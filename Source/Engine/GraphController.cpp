#include "GraphController.h"
#include "../PluginWindow.h"

GraphController::GraphController (AudioProcessorGraph& g, AudioPluginFormatManager& fm)
    : graph (g), formatManager (fm)
{
}

//==============================================================================
void GraphController::loadFrom (PropertiesFile& settings)
{
    const String xml = settings.getValue ("graphDocument");
    bool loaded = false;

    if (xml.isNotEmpty())
    {
        GraphDocument doc = GraphDocument::fromXml (xml);
        if (doc.isValid())
        {
            document = std::move (doc);
            loaded = true;
        }
    }

    if (! loaded)
    {
        // First run with the new format: import the legacy serial-chain settings.
        // Legacy keys are left untouched so downgrading remains possible.
        document = GraphDocument::migrateFromLegacySettings (settings);
    }

    rebuild();
}

void GraphController::save (PropertiesFile& settings)
{
    captureStates();
    settings.setValue ("graphDocument", document.toXml());
    settings.saveIfNeeded();
}

//==============================================================================
std::vector<String> GraphController::getChainUids() const
{
    std::vector<String> chain;
    const String inUid  = document.getIoNodeUid (true);
    const String outUid = document.getIoNodeUid (false);

    chain.push_back (inUid);

    // Follow channel-0 connections from input to output.
    ValueTree connections = document.getConnections();
    String current = inUid;
    const int maxSteps = document.getNodes().getNumChildren() + 1;

    for (int step = 0; step < maxSteps && current != outUid; ++step)
    {
        String next;
        for (int i = 0; i < connections.getNumChildren(); ++i)
        {
            ValueTree c = connections.getChild (i);
            if (c.getProperty ("srcUid").toString() == current
                && (int) c.getProperty ("srcCh") == 0)
            {
                next = c.getProperty ("dstUid").toString();
                break;
            }
        }
        if (next.isEmpty())
            break;
        chain.push_back (next);
        current = next;
    }

    if (chain.back() != outUid)
    {
        // Malformed/non-serial document — fall back to document node order.
        chain.clear();
        chain.push_back (inUid);
        ValueTree nodes = document.getNodes();
        for (int i = 0; i < nodes.getNumChildren(); ++i)
        {
            ValueTree node = nodes.getChild (i);
            if (node.getProperty ("type").toString() == GraphDocument::typePlugin)
                chain.push_back (node.getProperty ("uid").toString());
        }
        chain.push_back (outUid);
    }

    return chain;
}

void GraphController::rewriteChainConnections (const std::vector<String>& chainUids)
{
    document.clearConnections();
    for (size_t i = 0; i + 1 < chainUids.size(); ++i)
    {
        document.addConnection (chainUids[i], 0, chainUids[i + 1], 0);
        document.addConnection (chainUids[i], 1, chainUids[i + 1], 1);
    }
    // NOTE: node canvas positions are user-owned (dragged on the canvas, persisted
    // per node). Do NOT rewrite them here — chain edits (add/remove/move) must
    // leave every existing node exactly where the user put it.
}

//==============================================================================
void GraphController::rebuild()
{
    PluginWindow::closeAllCurrentlyOpenWindows();
    graph.clear();
    uidToNodeId.clear();
    inputMeter = outputMeter = nullptr;
    nodeStrips.clear();
    nodeMixes.clear();
    midiInputNode = nullptr;
    monoNode = nullptr;
    missingPlugins.clear();

    using IOProcessor = AudioProcessorGraph::AudioGraphIOProcessor;

    ValueTree nodes = document.getNodes();
    for (int i = 0; i < nodes.getNumChildren(); ++i)
    {
        ValueTree nodeTree = nodes.getChild (i);
        const String uid  = nodeTree.getProperty ("uid").toString();
        const String type = nodeTree.getProperty ("type").toString();

        std::unique_ptr<AudioProcessor> processor;

        if (type == GraphDocument::typeAudioIn)
            processor = std::make_unique<IOProcessor> (IOProcessor::audioInputNode);
        else if (type == GraphDocument::typeAudioOut)
            processor = std::make_unique<IOProcessor> (IOProcessor::audioOutputNode);
        else if (type == GraphDocument::typePlugin)
        {
            std::unique_ptr<AudioProcessor> instance;
            if (auto desc = GraphDocument::getPluginDescription (nodeTree))
            {
                String errorMessage;
                if (auto created = formatManager.createPluginInstance (*desc, graph.getSampleRate(),
                                                                       graph.getBlockSize(), errorMessage))
                {
                    // Promote mono-defaulting plugins (e.g. MAutoPitch) to stereo before
                    // restoring state, so they don't collapse a channel in the stereo chain.
                    lighthost::buses::forceStereo (*created, graph.getSampleRate(), graph.getBlockSize());

                    const String stateB64 = nodeTree.getProperty ("state").toString();
                    MemoryBlock stateBinary;
                    if (stateBinary.fromBase64Encoding (stateB64) && stateBinary.getSize() > 0)
                        created->setStateInformation (stateBinary.getData(), (int) stateBinary.getSize());
                    instance = std::move (created);
                }
            }

            if (instance != nullptr)
            {
                processor = std::move (instance);
            }
            else
            {
                // Plugin missing/broken: keep the node as a pass-through so it does
                // NOT sever the chain. The card still shows "missing", and the saved
                // plugin state is preserved (captureStates skips missing nodes).
                processor = std::make_unique<PassThroughProcessor>();
                missingPlugins.insert (uid);
            }
        }

        if (processor != nullptr)
        {
            if (auto node = graph.addNode (std::move (processor)))
            {
                node->setBypassed ((bool) nodeTree.getProperty ("bypassed", false));
                uidToNodeId[uid] = node->nodeID;
            }
        }
    }

    ValueTree connections = document.getConnections();
    for (int i = 0; i < connections.getNumChildren(); ++i)
    {
        ValueTree c = connections.getChild (i);
        const auto src = uidToNodeId.find (c.getProperty ("srcUid").toString());
        const auto dst = uidToNodeId.find (c.getProperty ("dstUid").toString());
        if (src != uidToNodeId.end() && dst != uidToNodeId.end())
            graph.addConnection ({ { src->second, (int) c.getProperty ("srcCh") },
                                   { dst->second, (int) c.getProperty ("dstCh") } });
    }

    insertNodeStrips();
    insertMasterMeters();
    insertInputConditioner();
    insertMidiRouting();
}

void GraphController::captureStates()
{
    ValueTree nodes = document.getNodes();
    for (int i = 0; i < nodes.getNumChildren(); ++i)
    {
        ValueTree nodeTree = nodes.getChild (i);
        if (nodeTree.getProperty ("type").toString() != GraphDocument::typePlugin)
            continue;
        if (missingPlugins.count (nodeTree.getProperty ("uid").toString()) > 0)
            continue;   // pass-through stand-in: keep the plugin's saved state intact

        if (auto* node = getNodeForUid (nodeTree.getProperty ("uid").toString()))
        {
            MemoryBlock stateBinary;
            node->getProcessor()->getStateInformation (stateBinary);
            nodeTree.setProperty ("state", stateBinary.toBase64Encoding(), nullptr);
        }
    }
}

//==============================================================================
std::vector<GraphController::ChainItem> GraphController::getChain() const
{
    std::vector<ChainItem> result;
    const std::vector<String> chain = getChainUids();

    for (size_t i = 1; i + 1 < chain.size(); ++i)   // skip audioIn/audioOut
    {
        ValueTree node = document.getNodeByUid (chain[i]);
        ChainItem item;
        item.uid      = chain[i];
        item.name     = node.getProperty ("name").toString();
        item.bypassed = (bool) node.getProperty ("bypassed", false);
        item.missing  = missingPlugins.count (item.uid) > 0;
        result.push_back (item);
    }
    return result;
}

String GraphController::appendToChain (const PluginDescription& desc)
{
    captureStates();
    std::vector<String> chain = getChainUids();

    // Place the new node to the right of the current rightmost plugin so it does
    // not stack on top of existing cards (positions are otherwise user-owned).
    float maxX = 0.15f;
    bool  anyPlugin = false;
    ValueTree nodes = document.getNodes();
    for (int i = 0; i < nodes.getNumChildren(); ++i)
    {
        const auto n = nodes.getChild (i);
        if (n.getProperty ("type").toString() == GraphDocument::typePlugin)
        {
            maxX = jmax (maxX, (float) n.getProperty ("x", 0.15f));
            anyPlugin = true;
        }
    }
    const float newX = anyPlugin ? jmin (0.9f, maxX + 0.12f) : 0.3f;
    const String uid = document.addPluginNode (desc, newX, 0.5f);
    chain.insert (chain.end() - 1, uid);   // before audioOut
    rewriteChainConnections (chain);
    rebuild();
    return uidToNodeId.count (uid) > 0 ? uid : String();
}

void GraphController::removeFromChain (const String& uid)
{
    captureStates();
    std::vector<String> chain = getChainUids();
    chain.erase (std::remove (chain.begin(), chain.end(), uid), chain.end());
    document.removeNode (uid);
    rewriteChainConnections (chain);
    rebuild();
}

void GraphController::moveUp (const String& uid)
{
    captureStates();
    std::vector<String> chain = getChainUids();
    for (size_t i = 2; i + 1 < chain.size(); ++i)   // first movable slot is index 2
    {
        if (chain[i] == uid)
        {
            std::swap (chain[i], chain[i - 1]);
            break;
        }
    }
    rewriteChainConnections (chain);
    rebuild();
}

void GraphController::moveDown (const String& uid)
{
    captureStates();
    std::vector<String> chain = getChainUids();
    for (size_t i = 1; i + 2 < chain.size(); ++i)
    {
        if (chain[i] == uid)
        {
            std::swap (chain[i], chain[i + 1]);
            break;
        }
    }
    rewriteChainConnections (chain);
    rebuild();
}

void GraphController::setBypassed (const String& uid, bool shouldBypass)
{
    document.setBypassed (uid, shouldBypass);
    if (auto* node = getNodeForUid (uid))
        node->setBypassed (shouldBypass);   // glitch-free pass-through, no rebuild
}

void GraphController::clearAllPluginStates()
{
    document.clearAllPluginStates();
    rebuild();   // fresh instances with default state
}

//==============================================================================
AudioProcessorGraph::Node* GraphController::getNodeForUid (const String& uid) const
{
    const auto it = uidToNodeId.find (uid);
    if (it == uidToNodeId.end())
        return nullptr;
    return graph.getNodeForId (it->second);
}

//==============================================================================
// Metering (runtime only). Meters are spliced into the live graph after the
// document has been rebuilt, so they never appear in getChain()/uidToNodeId and
// never touch persistence. Splicing preserves per-channel routing.

void GraphController::insertMasterMeters()
{
    inputMeter  = nullptr;
    outputMeter = nullptr;

    const auto itIn  = uidToNodeId.find (document.getIoNodeUid (true));
    const auto itOut = uidToNodeId.find (document.getIoNodeUid (false));

    // Output meter first: measures exactly what reaches the audio output.
    if (itOut != uidToNodeId.end())
        outputMeter = spliceMeterBefore (itOut->second);

    // Input meter: measures the dry signal leaving the audio input.
    if (itIn != uidToNodeId.end())
        inputMeter = spliceMeterAfter (itIn->second);
}

// Redirect every X -> target connection through a new meter: X -> meter -> target.
MeterProcessor* GraphController::spliceMeterBefore (AudioProcessorGraph::NodeID target)
{
    auto meterNode = graph.addNode (std::make_unique<MeterProcessor>());
    if (meterNode == nullptr)
        return nullptr;

    const auto meterId = meterNode->nodeID;

    for (const auto& c : graph.getConnections())
    {
        if (c.destination.nodeID == target)
        {
            graph.removeConnection (c);
            graph.addConnection ({ c.source, { meterId, c.destination.channelIndex } });
        }
    }

    for (int ch = 0; ch < MeterTap::numChannels; ++ch)
        graph.addConnection ({ { meterId, ch }, { target, ch } });

    return dynamic_cast<MeterProcessor*> (meterNode->getProcessor());
}

// Redirect every source -> Y connection through a new meter: source -> meter -> Y.
MeterProcessor* GraphController::spliceMeterAfter (AudioProcessorGraph::NodeID source)
{
    auto meterNode = graph.addNode (std::make_unique<MeterProcessor>());
    if (meterNode == nullptr)
        return nullptr;

    const auto meterId = meterNode->nodeID;

    for (const auto& c : graph.getConnections())
    {
        if (c.source.nodeID == source && c.destination.nodeID != meterId)
        {
            graph.removeConnection (c);
            graph.addConnection ({ { meterId, c.source.channelIndex }, c.destination });
        }
    }

    for (int ch = 0; ch < MeterTap::numChannels; ++ch)
        graph.addConnection ({ { source, ch }, { meterId, ch } });

    return dynamic_cast<MeterProcessor*> (meterNode->getProcessor());
}

//==============================================================================
// Mono input (runtime only). When enabled, a MonoInputProcessor is spliced right
// after the audio-input node so everything downstream (input meter, plugins,
// output) sees the input summed to mono on both channels.

void GraphController::insertInputConditioner()
{
    monoNode = nullptr;
    if (! monoInput)
        return;

    const auto itIn = uidToNodeId.find (document.getIoNodeUid (true));
    if (itIn != uidToNodeId.end())
        monoNode = spliceMonoAfter (itIn->second);
}

// Redirect every source -> Y connection through a new mono node: source -> mono -> Y.
// Mirrors spliceMeterAfter but folds the stereo pair to mono.
MonoInputProcessor* GraphController::spliceMonoAfter (AudioProcessorGraph::NodeID source)
{
    auto monoGraphNode = graph.addNode (std::make_unique<MonoInputProcessor>());
    if (monoGraphNode == nullptr)
        return nullptr;

    const auto monoId = monoGraphNode->nodeID;

    for (const auto& c : graph.getConnections())
    {
        if (c.source.nodeID == source && c.destination.nodeID != monoId)
        {
            graph.removeConnection (c);
            graph.addConnection ({ { monoId, c.source.channelIndex }, c.destination });
        }
    }

    for (int ch = 0; ch < MeterTap::numChannels; ++ch)
        graph.addConnection ({ { source, ch }, { monoId, ch } });

    return dynamic_cast<MonoInputProcessor*> (monoGraphNode->getProcessor());
}

void GraphController::setMonoInput (bool shouldSumToMono)
{
    if (monoInput == shouldSumToMono)
        return;
    monoInput = shouldSumToMono;
    rewireConnections();   // re-splice runtime nodes live (no plugin reload)
}

const MeterTap* GraphController::getInputMeter() const noexcept
{
    return inputMeter != nullptr ? &inputMeter->getTap() : nullptr;
}

const MeterTap* GraphController::getOutputMeter() const noexcept
{
    return outputMeter != nullptr ? &outputMeter->getTap() : nullptr;
}

//==============================================================================
// Per-node channel strips (M4). Like the master meters, strips are runtime nodes
// spliced into the live graph after the document is built; the plugin instance
// stays the node in uidToNodeId (so state capture / editors are unaffected).
// Their gain/pan values persist in the document; the meter is runtime-only.

void GraphController::insertNodeStrips()
{
    nodeStrips.clear();
    nodeMixes.clear();

    ValueTree nodes = document.getNodes();
    for (int i = 0; i < nodes.getNumChildren(); ++i)
    {
        ValueTree n = nodes.getChild (i);
        if (n.getProperty ("type").toString() != GraphDocument::typePlugin)
            continue;

        const String uid = n.getProperty ("uid").toString();
        const auto it = uidToNodeId.find (uid);
        if (it == uidToNodeId.end())
            continue;   // plugin missing/broken: no live node, so no strip

        auto* stripNode = spliceStripAfter (it->second, document.getGain (uid), document.getPan (uid));
        if (stripNode == nullptr)
            continue;

        if (auto* strip = dynamic_cast<NodeStripProcessor*> (stripNode->getProcessor()))
            nodeStrips[uid] = strip;

        // Dry/wet: only splice the parallel dry branch when the node is not fully
        // wet, so a default graph (all mix == 1) is byte-for-byte the old topology.
        const float mix = document.getMix (uid);
        if (mix < 1.0f)
            insertMixNode (it->second, stripNode->nodeID, uid, mix);
    }
}

// source -> strip -> (old destinations of source). Mirrors spliceMeterAfter but
// carries the node's gain/pan. Returns the live strip node (for the mix splice).
AudioProcessorGraph::Node* GraphController::spliceStripAfter (AudioProcessorGraph::NodeID source, float gain, float pan)
{
    auto stripNode = graph.addNode (std::make_unique<NodeStripProcessor>());
    if (stripNode == nullptr)
        return nullptr;

    const auto stripId = stripNode->nodeID;

    for (const auto& c : graph.getConnections())
    {
        if (c.source.nodeID == source && c.destination.nodeID != stripId)
        {
            graph.removeConnection (c);
            graph.addConnection ({ { stripId, c.source.channelIndex }, c.destination });
        }
    }

    for (int ch = 0; ch < MeterTap::numChannels; ++ch)
        graph.addConnection ({ { source, ch }, { stripId, ch } });

    if (auto* strip = dynamic_cast<NodeStripProcessor*> (stripNode->getProcessor()))
    {
        strip->setGain (gain);
        strip->setPan  (pan);
    }
    return stripNode.get();
}

// Insert a dry/wet mix node between a plugin and its (already-spliced) strip:
//
//   before:  [srcs] -> plugin -> strip -> ...
//   after:   [srcs] -> plugin --wet--> mix -> strip -> ...
//                \-----------------dry--> mix
//
// The plugin's current INPUT connections are its true dry sources (the strip
// splice only touched the plugin's OUTPUTS, so they are still intact here). We
// read them live rather than from the document, so the dry tap always reflects
// whatever actually feeds the plugin -- including upstream strips, which get
// rerouted onto this dry edge automatically when they are spliced.
void GraphController::insertMixNode (AudioProcessorGraph::NodeID pluginId,
                                     AudioProcessorGraph::NodeID stripId,
                                     const String& uid, float mix)
{
    auto mixNode = graph.addNode (std::make_unique<MixProcessor>());
    if (mixNode == nullptr)
        return;

    const auto mixId = mixNode->nodeID;

    // Capture the plugin's dry input sources (audio channels 0/1 only -- MIDI is
    // spliced later and uses a sentinel channel index).
    std::vector<AudioProcessorGraph::Connection> dryInputs;
    for (const auto& c : graph.getConnections())
        if (c.destination.nodeID == pluginId
            && c.destination.channelIndex >= 0 && c.destination.channelIndex < MeterTap::numChannels)
            dryInputs.push_back (c);

    // Reroute plugin -> strip  into  plugin -> mix(wet 0,1) and mix -> strip.
    for (const auto& c : graph.getConnections())
    {
        if (c.source.nodeID == pluginId && c.destination.nodeID == stripId)
            graph.removeConnection (c);
    }
    for (int ch = 0; ch < MeterTap::numChannels; ++ch)
    {
        graph.addConnection ({ { pluginId, ch }, { mixId,   ch } });   // wet in
        graph.addConnection ({ { mixId,    ch }, { stripId, ch } });   // blended out
    }

    // Dry branch: the plugin's own inputs, in parallel, into mix channels 2,3.
    for (const auto& c : dryInputs)
        graph.addConnection ({ c.source, { mixId, c.destination.channelIndex + MeterTap::numChannels } });

    if (auto* m = dynamic_cast<MixProcessor*> (mixNode->getProcessor()))
    {
        m->setMix (mix);
        nodeMixes[uid] = m;
    }
}

void GraphController::setNodeGain (const String& uid, float gain)
{
    document.setGain (uid, gain);
    const auto it = nodeStrips.find (uid);
    if (it != nodeStrips.end() && it->second != nullptr)
        it->second->setGain (gain);   // live, no rebuild
}

float GraphController::getNodeGain (const String& uid) const
{
    return document.getGain (uid);
}

void GraphController::setNodePan (const String& uid, float pan)
{
    document.setPan (uid, pan);
    const auto it = nodeStrips.find (uid);
    if (it != nodeStrips.end() && it->second != nullptr)
        it->second->setPan (pan);   // live, no rebuild
}

float GraphController::getNodePan (const String& uid) const
{
    return document.getPan (uid);
}

const MeterTap* GraphController::getNodeMeter (const String& uid) const noexcept
{
    const auto it = nodeStrips.find (uid);
    return (it != nodeStrips.end() && it->second != nullptr) ? &it->second->getTap() : nullptr;
}

void GraphController::setNodeMix (const String& uid, float mix)
{
    mix = jlimit (0.0f, 1.0f, mix);
    document.setMix (uid, mix);

    const auto it = nodeMixes.find (uid);
    if (it != nodeMixes.end() && it->second != nullptr)
    {
        it->second->setMix (mix);   // branch exists: apply live, no rewire
    }
    else if (mix < 1.0f)
    {
        // No dry branch yet and the node is no longer fully wet: build it. A
        // rewire re-splices runtime nodes only -- plugins are not reloaded.
        rewireConnections();
    }
    // No branch + mix == 1.0: nothing to do (already pure wet).
}

float GraphController::getNodeMix (const String& uid) const
{
    return document.getMix (uid);
}

//==============================================================================
// Plugin parameter bridge (M5). Delegates to the testable helpers in
// PluginParameters.h against the node's live plugin instance.

std::vector<lighthost::params::ParamInfo> GraphController::getNodeParameters (const String& uid, bool automatableOnly) const
{
    if (auto* node = getNodeForUid (uid))
        if (auto* proc = node->getProcessor())
            return lighthost::params::collect (*proc, automatableOnly);
    return {};
}

std::vector<lighthost::params::ParamInfo> GraphController::getNodeCardParameters (const String& uid, int count) const
{
    if (auto* node = getNodeForUid (uid))
        if (auto* proc = node->getProcessor())
            return lighthost::params::curate (*proc, count);
    return {};
}

void GraphController::setNodeParameter (const String& uid, int index, float normalized)
{
    if (auto* node = getNodeForUid (uid))
        if (auto* proc = node->getProcessor())
            lighthost::params::setValue (*proc, index, normalized);   // live, no rebuild
}

float GraphController::getNodeParameter (const String& uid, int index) const
{
    if (auto* node = getNodeForUid (uid))
        if (auto* proc = node->getProcessor())
            return lighthost::params::getValue (*proc, index);
    return 0.0f;
}

String GraphController::getNodeParameterText (const String& uid, int index) const
{
    if (auto* node = getNodeForUid (uid))
        if (auto* proc = node->getProcessor())
            return lighthost::params::valueText (*proc, index);
    return {};
}

//==============================================================================
// Arbitrary routing (M6). Re-derive only the connection topology + runtime
// strip/meter nodes from the document, keeping plugin/IO nodes (and their live
// instances) intact — so editing a wire never reloads a plugin.

void GraphController::rewireConnections()
{
    // Drop the runtime strip/meter nodes; they get re-spliced below. Plugin/IO
    // nodes (in uidToNodeId) are left untouched.
    std::vector<AudioProcessorGraph::NodeID> runtimeNodes;
    for (auto* node : graph.getNodes())
        if (dynamic_cast<NodeStripProcessor*> (node->getProcessor()) != nullptr
         || dynamic_cast<MeterProcessor*>     (node->getProcessor()) != nullptr
         || dynamic_cast<MonoInputProcessor*> (node->getProcessor()) != nullptr
         || dynamic_cast<MixProcessor*>       (node->getProcessor()) != nullptr)
            runtimeNodes.push_back (node->nodeID);

    for (auto id : runtimeNodes)
        graph.removeNode (id);

    if (midiInputNode != nullptr)
    {
        graph.removeNode (midiInputNode->nodeID);
        midiInputNode = nullptr;
    }

    for (const auto& c : graph.getConnections())
        graph.removeConnection (c);

    ValueTree connections = document.getConnections();
    for (int i = 0; i < connections.getNumChildren(); ++i)
    {
        ValueTree c = connections.getChild (i);
        const auto src = uidToNodeId.find (c.getProperty ("srcUid").toString());
        const auto dst = uidToNodeId.find (c.getProperty ("dstUid").toString());
        if (src != uidToNodeId.end() && dst != uidToNodeId.end())
            graph.addConnection ({ { src->second, (int) c.getProperty ("srcCh") },
                                   { dst->second, (int) c.getProperty ("dstCh") } });
    }

    insertNodeStrips();
    insertMasterMeters();
    insertInputConditioner();
    insertMidiRouting();
}

bool GraphController::connect (const String& srcUid, int srcCh, const String& dstUid, int dstCh)
{
    if (! document.canAddConnection (srcUid, srcCh, dstUid, dstCh))
        return false;

    document.addConnection (srcUid, srcCh, dstUid, dstCh);
    rewireConnections();
    return true;
}

void GraphController::disconnect (const String& srcUid, int srcCh, const String& dstUid, int dstCh)
{
    document.removeConnection (srcUid, srcCh, dstUid, dstCh);
    rewireConnections();
}

bool GraphController::canConnect (const String& srcUid, int srcCh, const String& dstUid, int dstCh) const
{
    return document.canAddConnection (srcUid, srcCh, dstUid, dstCh);
}

std::vector<GraphController::Connection> GraphController::getConnections() const
{
    std::vector<Connection> out;
    ValueTree conns = document.getConnections();
    for (int i = 0; i < conns.getNumChildren(); ++i)
    {
        ValueTree c = conns.getChild (i);
        out.push_back ({ c.getProperty ("srcUid").toString(), (int) c.getProperty ("srcCh"),
                         c.getProperty ("dstUid").toString(), (int) c.getProperty ("dstCh") });
    }
    return out;
}

void GraphController::setNodePosition (const String& uid, float x, float y)
{
    document.setNodePosition (uid, x, y);   // GUI-only; no audio change, no rewire
}

float GraphController::getNodeX (const String& uid) const { return document.getNodeX (uid); }
float GraphController::getNodeY (const String& uid) const { return document.getNodeY (uid); }

bool GraphController::isLinearChain() const { return document.isLinearChain(); }

//==============================================================================
// MIDI routing (M7). Runtime-only, like the meters/strips: a midiInputNode is
// added and connected (on midiChannelIndex) to every plugin node that both
// accepts MIDI and has its per-node receivesMidi flag set. Added only when at
// least one such target exists, so a pure-audio graph carries no MIDI plumbing.

void GraphController::insertMidiRouting()
{
    midiInputNode = nullptr;
    using IOProcessor = AudioProcessorGraph::AudioGraphIOProcessor;

    std::vector<AudioProcessorGraph::NodeID> targets;
    for (const auto& entry : uidToNodeId)
    {
        if (! document.getReceivesMidi (entry.first))
            continue;
        if (auto* n = graph.getNodeForId (entry.second))
            if (auto* proc = n->getProcessor())
                if (proc->acceptsMidi())
                    targets.push_back (entry.second);
    }

    if (targets.empty())
        return;

    midiInputNode = graph.addNode (std::make_unique<IOProcessor> (IOProcessor::midiInputNode));
    if (midiInputNode == nullptr)
        return;

    for (auto t : targets)
        graph.addConnection ({ { midiInputNode->nodeID, AudioProcessorGraph::midiChannelIndex },
                               { t,                      AudioProcessorGraph::midiChannelIndex } });
}

void GraphController::setNodeReceivesMidi (const String& uid, bool b)
{
    document.setReceivesMidi (uid, b);
    rewireConnections();          // re-splice MIDI edges live (no plugin reload)
}

bool GraphController::getNodeReceivesMidi (const String& uid) const
{
    return document.getReceivesMidi (uid);
}

bool GraphController::nodeAcceptsMidi (const String& uid) const
{
    if (auto* n = getNodeForUid (uid))
        if (auto* p = n->getProcessor())
            return p->acceptsMidi();
    return false;
}

//==============================================================================
void GraphController::loadDocument (const GraphDocument& doc)
{
    document.state = doc.state.createCopy();   // deep copy: never share the store's tree
    rebuild();
}

GraphDocument GraphController::snapshotDocument()
{
    captureStates();                           // fold live plugin states into the document
    GraphDocument g;
    g.state = document.state.createCopy();
    return g;
}
