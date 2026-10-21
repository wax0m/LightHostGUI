#include "GraphController.h"
#include "../PluginWindow.h"
#include "SettingsBackup.h"

GraphController::GraphController (AudioProcessorGraph& g, AudioPluginFormatManager& fm)
    : graph (g), formatManager (fm)
{
}

//==============================================================================
void GraphController::loadFrom (PropertiesFile& settings)
{
    const String xml = settings.getValue ("graphDocument");
    bool loaded = false;
    loadWarning = {};

    if (xml.isNotEmpty())
    {
        GraphDocument doc = GraphDocument::fromXml (xml);
        if (doc.isValid())
        {
            document = std::move (doc);
            loaded = true;
        }
        else
        {
            // The next save() would overwrite the unreadable value with the fallback below.
            const auto backup = lighthost::backupUnreadable (settings, "graphDocument", xml);
            loadWarning = "Your saved graph could not be read. "
                        + (backup != File() ? "A copy was kept at " + backup.getFullPathName() + ". "
                                            : String ("It could not be copied either. "))
                        + "The app started with defaults.";
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

    // Fall back to document node order when the walk dead-ended OR reached the
    // output without visiting every plugin (branched graph / unconnected nodes) —
    // the tray list must never silently omit a live plugin.
    int numPluginNodes = 0;
    ValueTree nodes = document.getNodes();
    for (int i = 0; i < nodes.getNumChildren(); ++i)
        if (nodes.getChild (i).getProperty ("type").toString() == GraphDocument::typePlugin)
            ++numPluginNodes;

    if (chain.back() != outUid || (int) chain.size() != numPluginNodes + 2)
    {
        chain.clear();
        chain.push_back (inUid);
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
// Batched graph mutation. AudioProcessorGraph defaults every add/remove/clear to
// UpdateKind::sync, which builds and publishes a render sequence to the audio thread
// per call (including half-wired intermediate topologies, even an all-disconnected
// one). These wrappers are the ONLY place graph.addNode/removeNode/addConnection/
// removeConnection/clear may be called; they never publish. A user gesture ends with
// exactly one publishTopology().
AudioProcessorGraph::Node::Ptr GraphController::addNodeBatched (std::unique_ptr<AudioProcessor> p)
{
    return graph.addNode (std::move (p), std::nullopt, AudioProcessorGraph::UpdateKind::none);
}

AudioProcessorGraph::Node::Ptr GraphController::removeNodeBatched (AudioProcessorGraph::NodeID id)
{
    return graph.removeNode (id, AudioProcessorGraph::UpdateKind::none);
}

bool GraphController::connectBatched (const AudioProcessorGraph::Connection& c)
{
    return graph.addConnection (c, AudioProcessorGraph::UpdateKind::none);
}

bool GraphController::disconnectBatched (const AudioProcessorGraph::Connection& c)
{
    return graph.removeConnection (c, AudioProcessorGraph::UpdateKind::none);
}

void GraphController::clearBatched()
{
    graph.clear (AudioProcessorGraph::UpdateKind::none);
}

void GraphController::publishTopology()
{
    graph.rebuild();
    ++publishCount;
}

//==============================================================================
// closeAllCurrentlyOpenWindows() below pumps the message loop for ~50 ms, so a
// queued edit (a preset switch via loadDocument, or clearAllPluginStates, dispatched
// during that window) can call rebuild() again while this one is mid-teardown. Coalesce: the reentrant call
// only flags rebuildPending, and we run one more full pass over the (already
// updated) document once the current pass finishes.
void GraphController::rebuild()
{
    if (rebuilding)
    {
        rebuildPending = true;
        return;
    }

    const ScopedValueSetter<bool> guard (rebuilding, true);
    do
    {
        rebuildPending = false;
        rebuildNow();
    } while (rebuildPending);
}

void GraphController::rebuildNow()
{
    // INVARIANT: the window-close pump below must stay the FIRST statement. It can
    // dispatch queued edits, which are only safe because the live graph and
    // uidToNodeId are still intact at that point — never move teardown above it.
    PluginWindow::closeAllCurrentlyOpenWindows();
    clearBatched();
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
            addPluginNodeToGraph (nodeTree);
            continue;
        }

        if (processor != nullptr)
        {
            if (auto node = addNodeBatched (std::move (processor)))
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
            connectBatched ({ { src->second, (int) c.getProperty ("srcCh") },
                                   { dst->second, (int) c.getProperty ("dstCh") } });
    }

    insertNodeStrips();
    insertMasterMeters();
    insertInputConditioner();
    insertMidiRouting();
    publishTopology();
}

// Instantiate one plugin document node into the live graph. On load failure the
// node becomes a PassThroughProcessor stand-in (so it does NOT sever the chain)
// and is recorded in missingPlugins; the saved plugin state is preserved because
// captureStates skips missing nodes.
AudioProcessorGraph::Node* GraphController::addPluginNodeToGraph (const ValueTree& nodeTree)
{
    return registerPluginNode (nodeTree, createPluginInstanceFor (nodeTree));
}

// Create the hosted plugin for a document node (stereo-forced, saved state
// restored), or nullptr when it can't be loaded. Touches nothing but the new
// instance, so the retry path can call it before deciding to change the graph.
std::unique_ptr<AudioProcessor> GraphController::createPluginInstanceFor (const ValueTree& nodeTree)
{
    const auto desc = GraphDocument::getPluginDescription (nodeTree);
    if (! desc.has_value())
        return nullptr;

    String errorMessage;
    auto created = formatManager.createPluginInstance (*desc, graph.getSampleRate(),
                                                       graph.getBlockSize(), errorMessage);
    if (created == nullptr)
        return nullptr;

    // Promote mono-defaulting plugins (e.g. MAutoPitch) to stereo before
    // restoring state, so they don't collapse a channel in the stereo chain.
    lighthost::buses::forceStereo (*created, graph.getSampleRate(), graph.getBlockSize());

    const String stateB64 = nodeTree.getProperty ("state").toString();
    MemoryBlock stateBinary;
    if (stateBinary.fromBase64Encoding (stateB64) && stateBinary.getSize() > 0)
        created->setStateInformation (stateBinary.getData(), (int) stateBinary.getSize());

    return std::unique_ptr<AudioProcessor> (std::move (created));
}

// Register an instance as the live node for a document node; a null instance
// becomes a PassThrough stand-in recorded in missingPlugins. Not published here:
// every caller follows with publishTopology() (rebuildNow) or rewireConnections().
AudioProcessorGraph::Node* GraphController::registerPluginNode (const ValueTree& nodeTree,
                                                                std::unique_ptr<AudioProcessor> instance)
{
    const String uid = nodeTree.getProperty ("uid").toString();

    if (instance == nullptr)
    {
        instance = std::make_unique<PassThroughProcessor>();
        missingPlugins.insert (uid);
    }

    if (auto node = addNodeBatched (std::move (instance)))
    {
        node->setBypassed ((bool) nodeTree.getProperty ("bypassed", false));
        uidToNodeId[uid] = node->nodeID;
        return node.get();
    }
    return nullptr;
}

// Teardown mirror of registerPluginNode. EVERY per-uid runtime map must be cleaned
// here (dry/wet's nodeMixes too, when it lands). Not published: the caller follows
// with rewireConnections(), which also drops and re-splices the runtime strip node.
void GraphController::removePluginNodeFromGraph (const String& uid)
{
    const auto it = uidToNodeId.find (uid);
    if (it != uidToNodeId.end())
    {
        PluginWindow::closeCurrentlyOpenWindowsFor (it->second);   // synchronous, no pump
        removeNodeBatched (it->second);
        uidToNodeId.erase (it);
    }
    missingPlugins.erase (uid);
    nodeStrips.erase (uid);
    nodeMixes.erase (uid);   // the mix node itself is a runtime node, swept by rewireConnections()
}

// Missing-plugin recovery (round-3 finding 6). Instantiate FIRST: if the plugin
// still can't load (or shows a modal dialog while loading), the live graph has not
// been touched and there is nothing to publish. On success the PassThrough stand-in
// is replaced under the same uid; the caller's rewireConnections() re-derives its
// edges and strip from the document. Other plugin instances are never touched.
bool GraphController::swapInRecoveredPlugin (const String& uid)
{
    if (! isMissing (uid))
        return false;

    const ValueTree nodeTree = document.getNodeByUid (uid);
    if (! nodeTree.isValid())
        return false;

    auto instance = createPluginInstanceFor (nodeTree);
    if (instance == nullptr)
        return false;

    removePluginNodeFromGraph (uid);
    const bool registered = registerPluginNode (nodeTree, std::move (instance)) != nullptr;
    jassert (registered);
    return registered;
}

bool GraphController::retryMissingPlugin (const String& uid)
{
    if (! swapInRecoveredPlugin (uid))
        return false;

    rewireConnections();   // the one publish for this gesture
    return true;
}

int GraphController::retryMissingPlugins()
{
    // Copy first: a successful swap erases its uid from missingPlugins.
    const std::vector<String> candidates (missingPlugins.begin(), missingPlugins.end());

    int recovered = 0;
    for (const auto& uid : candidates)
        if (swapInRecoveredPlugin (uid))
            ++recovered;

    if (recovered > 0)
        rewireConnections();   // one publish for the whole batch
    return recovered;
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
        item.missing  = isNodeUnavailable (item.uid);
        result.push_back (item);
    }
    return result;
}

// Place a new node to the right of the current rightmost plugin, then walk down
// (and finally left) past any occupied slot — two cards must never land on the
// same spot, or the later one is invisible (positions are otherwise user-owned).
juce::Point<float> GraphController::placementForNewNode() const
{
    ValueTree nodes = document.getNodes();

    float maxX = 0.15f;
    bool  anyPlugin = false;
    for (int i = 0; i < nodes.getNumChildren(); ++i)
    {
        const auto n = nodes.getChild (i);
        if (n.getProperty ("type").toString() == GraphDocument::typePlugin)
        {
            // Positions go through the document accessors so the default for a node
            // without x/y lives in one place (GraphDocument::getNodeX/getNodeY).
            maxX = jmax (maxX, document.getNodeX (n.getProperty ("uid").toString()));
            anyPlugin = true;
        }
    }

    const auto occupied = [this, &nodes] (float px, float py)
    {
        for (int i = 0; i < nodes.getNumChildren(); ++i)
        {
            const auto n = nodes.getChild (i);
            if (n.getProperty ("type").toString() != GraphDocument::typePlugin)
                continue;
            const String uid = n.getProperty ("uid").toString();
            if (std::abs (document.getNodeX (uid) - px) < 0.05f
             && std::abs (document.getNodeY (uid) - py) < 0.05f)
                return true;
        }
        return false;
    };

    float x = anyPlugin ? jmin (0.9f, maxX + 0.12f) : 0.3f;
    float y = 0.5f;
    for (int guard = 0; guard < 64 && occupied (x, y); ++guard)
    {
        y += 0.12f;                                     // cascade down…
        if (y > 0.9f)
        {
            y = 0.1f;                                   // …then wrap to the top
            x = jmax (0.1f, x - 0.06f);                 // one half-step left per column
        }
    }
    return { x, y };
}

String GraphController::addPlugin (const PluginDescription& desc)
{
    // Serial append rewrites ALL connections as a chain, which would destroy
    // parallel routing — on a branched graph the node is added unconnected instead.
    return isLinearChain() ? appendToChain (desc) : addNodeUnconnected (desc);
}

String GraphController::appendToChain (const PluginDescription& desc)
{
    std::vector<String> chain = getChainUids();

    const auto pos = placementForNewNode();
    const String uid = document.addPluginNode (desc, pos.x, pos.y);
    chain.insert (chain.end() - 1, uid);   // before audioOut
    rewriteChainConnections (chain);

    // Instantiate ONLY the new plugin; existing instances (and their editors) are
    // untouched, then one live rewire re-derives connections and runtime strips.
    addPluginNodeToGraph (document.getNodeByUid (uid));
    rewireConnections();
    return uidToNodeId.count (uid) > 0 ? uid : String();
}

String GraphController::addNodeUnconnected (const PluginDescription& desc)
{
    const auto pos = placementForNewNode();
    const String uid = document.addPluginNode (desc, pos.x, pos.y);
    addPluginNodeToGraph (document.getNodeByUid (uid));
    rewireConnections();   // existing connections are untouched — nothing is rewritten serially
    return uidToNodeId.count (uid) > 0 ? uid : String();
}

// Remove one node and heal: every upstream edge is bridged to every downstream
// edge that met it with channel continuity through the node, and NOTHING else is
// rewritten — a delete on a branched graph keeps all other routing intact. On a
// linear chain A -> X -> B this heals to A -> B. Only the deleted node's live
// objects are torn down: no plugin is reloaded and no other editor is closed.
void GraphController::removeNode (const String& uid)
{
    std::vector<Connection> ins;    // X -> node, verbatim
    std::vector<Connection> outs;   // node -> Y, verbatim

    for (const auto& c : getConnections())
    {
        if (c.dstUid == uid)
            ins.push_back (c);
        else if (c.srcUid == uid)
            outs.push_back (c);
    }

    document.removeNode (uid);   // also drops the node's own connections

    // Bridge (in.src, in.srcCh) -> (out.dst, out.dstCh) where the channel continues through
    // the node (in.dstCh == out.srcCh). canAddConnection dedupes an already-existing parallel
    // edge; healing cannot create a cycle because the path src -> node -> dst existed before.
    for (const auto& in : ins)
        for (const auto& out : outs)
            if (in.dstCh == out.srcCh
                && document.canAddConnection (in.srcUid, in.srcCh, out.dstUid, out.dstCh))
                document.addConnection (in.srcUid, in.srcCh, out.dstUid, out.dstCh);

    removePluginNodeFromGraph (uid);   // published by rewireConnections() below

    rewireConnections();   // re-derives edges + runtime strips; instances untouched
}

void GraphController::moveUp (const String& uid)
{
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
    rewireConnections();   // connections-only edit: no plugin reload
}

void GraphController::moveDown (const String& uid)
{
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
    rewireConnections();   // connections-only edit: no plugin reload
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
    {
        outputMeter = spliceMeterBefore (itOut->second);

        // Safety limiter goes in front of the output meter, so the meter shows what
        // actually reaches the device: ... -> guard -> meter -> audioOut.
        if (outputMeter != nullptr)
            for (auto* n : graph.getNodes())
                if (n->getProcessor() == outputMeter)
                {
                    spliceGuardBefore (n->nodeID);
                    break;
                }
    }

    // Input meter: measures the dry signal leaving the audio input.
    if (itIn != uidToNodeId.end())
        inputMeter = spliceMeterAfter (itIn->second);
}

// Redirect every X -> target connection through a new node: X -> node -> target.
template <typename Proc>
AudioProcessorGraph::Node* GraphController::spliceProcBefore (AudioProcessorGraph::NodeID target)
{
    auto node = addNodeBatched (std::make_unique<Proc>());
    if (node == nullptr)
        return nullptr;

    const auto nodeId = node->nodeID;

    for (const auto& c : graph.getConnections())
    {
        if (c.destination.nodeID == target)
        {
            disconnectBatched (c);
            connectBatched ({ c.source, { nodeId, c.destination.channelIndex } });
        }
    }

    for (int ch = 0; ch < MeterTap::numChannels; ++ch)
        connectBatched ({ { nodeId, ch }, { target, ch } });

    return node.get();
}

MeterProcessor* GraphController::spliceMeterBefore (AudioProcessorGraph::NodeID target)
{
    auto* node = spliceProcBefore<MeterProcessor> (target);
    return node != nullptr ? dynamic_cast<MeterProcessor*> (node->getProcessor()) : nullptr;
}

OutputGuardProcessor* GraphController::spliceGuardBefore (AudioProcessorGraph::NodeID target)
{
    auto* node = spliceProcBefore<OutputGuardProcessor> (target);
    return node != nullptr ? dynamic_cast<OutputGuardProcessor*> (node->getProcessor()) : nullptr;
}

// Redirect every source -> Y connection through a new meter: source -> meter -> Y.
MeterProcessor* GraphController::spliceMeterAfter (AudioProcessorGraph::NodeID source)
{
    auto meterNode = addNodeBatched (std::make_unique<MeterProcessor>());
    if (meterNode == nullptr)
        return nullptr;

    const auto meterId = meterNode->nodeID;

    for (const auto& c : graph.getConnections())
    {
        if (c.source.nodeID == source && c.destination.nodeID != meterId)
        {
            disconnectBatched (c);
            connectBatched ({ { meterId, c.source.channelIndex }, c.destination });
        }
    }

    for (int ch = 0; ch < MeterTap::numChannels; ++ch)
        connectBatched ({ { source, ch }, { meterId, ch } });

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
    auto monoGraphNode = addNodeBatched (std::make_unique<MonoInputProcessor>());
    if (monoGraphNode == nullptr)
        return nullptr;

    const auto monoId = monoGraphNode->nodeID;

    for (const auto& c : graph.getConnections())
    {
        if (c.source.nodeID == source && c.destination.nodeID != monoId)
        {
            disconnectBatched (c);
            connectBatched ({ { monoId, c.source.channelIndex }, c.destination });
        }
    }

    for (int ch = 0; ch < MeterTap::numChannels; ++ch)
        connectBatched ({ { source, ch }, { monoId, ch } });

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
            continue;   // no live node at all (registration failed); a missing plugin's
                        // PassThrough stand-in IS live and gets a strip like any plugin

        auto* stripNode = spliceStripAfter (it->second, document.getGain (uid), document.getPan (uid));
        if (stripNode == nullptr)
            continue;

        if (auto* strip = dynamic_cast<NodeStripProcessor*> (stripNode->getProcessor()))
            nodeStrips[uid] = strip;

        // Dry/wet: the parallel dry branch exists only while the node is not fully
        // wet, so a default graph (every mix == 1) keeps the plain topology.
        const float mix = document.getMix (uid);
        if (mix < 1.0f)
            insertMixNode (it->second, stripNode->nodeID, uid, mix);
    }
}

// source -> strip -> (old destinations of source). Mirrors spliceMeterAfter but
// carries the node's gain/pan. Returns the live strip node (for the mix splice).
AudioProcessorGraph::Node* GraphController::spliceStripAfter (AudioProcessorGraph::NodeID source, float gain, float pan)
{
    auto stripNode = addNodeBatched (std::make_unique<NodeStripProcessor>());
    if (stripNode == nullptr)
        return nullptr;

    const auto stripId = stripNode->nodeID;

    for (const auto& c : graph.getConnections())
    {
        if (c.source.nodeID == source && c.destination.nodeID != stripId)
        {
            disconnectBatched (c);
            connectBatched ({ { stripId, c.source.channelIndex }, c.destination });
        }
    }

    for (int ch = 0; ch < MeterTap::numChannels; ++ch)
        connectBatched ({ { source, ch }, { stripId, ch } });

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
// The plugin's current INPUT connections are its true dry sources (the strip splice
// only touched the plugin's OUTPUTS). They are read from the live graph rather than
// the document, so the dry tap follows whatever really feeds the plugin: an upstream
// strip, master meter or mono node spliced later reroutes this dry edge too, because
// those splices move ALL of their source's outputs. All mutations are batched; the
// caller (rewireConnections / rebuildNow) publishes once.
void GraphController::insertMixNode (AudioProcessorGraph::NodeID pluginId,
                                     AudioProcessorGraph::NodeID stripId,
                                     const String& uid, float mix)
{
    auto mixNode = addNodeBatched (std::make_unique<MixProcessor>());
    if (mixNode == nullptr)
        return;

    const auto mixId = mixNode->nodeID;

    // The plugin's dry input sources (audio channels 0/1 only; MIDI is spliced later
    // and uses a sentinel channel index).
    std::vector<AudioProcessorGraph::Connection> dryInputs;
    for (const auto& c : graph.getConnections())
        if (c.destination.nodeID == pluginId
            && c.destination.channelIndex >= 0 && c.destination.channelIndex < MeterTap::numChannels)
            dryInputs.push_back (c);

    // Reroute plugin -> strip  into  plugin -> mix (wet 0,1) and mix -> strip.
    for (const auto& c : graph.getConnections())
        if (c.source.nodeID == pluginId && c.destination.nodeID == stripId)
            disconnectBatched (c);

    for (int ch = 0; ch < MeterTap::numChannels; ++ch)
    {
        connectBatched ({ { pluginId, ch }, { mixId,   ch } });   // wet in
        connectBatched ({ { mixId,    ch }, { stripId, ch } });   // blended out
    }

    // Dry branch: the plugin's own inputs, in parallel, into mix channels 2,3.
    for (const auto& c : dryInputs)
        connectBatched ({ c.source, { mixId, c.destination.channelIndex + MeterTap::numChannels } });

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

    if (uidToNodeId.count (uid) == 0)
        return;   // no live node: the persisted value is all there is to update

    const auto it = nodeMixes.find (uid);
    const bool hasBranch  = it != nodeMixes.end() && it->second != nullptr;
    const bool wantBranch = mix < 1.0f;

    if (hasBranch && wantBranch)
        it->second->setMix (mix);   // live via the atomic: no rewire, no publish
    else if (hasBranch != wantBranch)
        rewireConnections();        // build (first drop below 100 %) or tear down (back at 100 %): one publish
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
    // Drop the runtime strip/meter/mono/mix nodes; they get re-spliced below. Plugin/IO
    // nodes (in uidToNodeId) are left untouched.
    std::vector<AudioProcessorGraph::NodeID> runtimeNodes;
    for (auto* node : graph.getNodes())
        if (dynamic_cast<NodeStripProcessor*> (node->getProcessor()) != nullptr
         || dynamic_cast<MeterProcessor*>     (node->getProcessor()) != nullptr
         || dynamic_cast<MonoInputProcessor*> (node->getProcessor()) != nullptr
         || dynamic_cast<MixProcessor*>       (node->getProcessor()) != nullptr
         || dynamic_cast<OutputGuardProcessor*> (node->getProcessor()) != nullptr)
            runtimeNodes.push_back (node->nodeID);

    for (auto id : runtimeNodes)
        removeNodeBatched (id);

    if (midiInputNode != nullptr)
    {
        removeNodeBatched (midiInputNode->nodeID);
        midiInputNode = nullptr;
    }

    for (const auto& c : graph.getConnections())
        disconnectBatched (c);

    ValueTree connections = document.getConnections();
    for (int i = 0; i < connections.getNumChildren(); ++i)
    {
        ValueTree c = connections.getChild (i);
        const auto src = uidToNodeId.find (c.getProperty ("srcUid").toString());
        const auto dst = uidToNodeId.find (c.getProperty ("dstUid").toString());
        if (src != uidToNodeId.end() && dst != uidToNodeId.end())
            connectBatched ({ { src->second, (int) c.getProperty ("srcCh") },
                                   { dst->second, (int) c.getProperty ("dstCh") } });
    }

    insertNodeStrips();
    insertMasterMeters();
    insertInputConditioner();
    insertMidiRouting();
    publishTopology();   // the only publish of this gesture
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

// Stereo-pair operations: the pair (ch0+ch1, straight) is the goal state, so a
// half pair (mono edge from a legacy/hand-edited document) can be COMPLETED by a
// new drag rather than being stuck invisible and unrepairable.
bool GraphController::canConnectPair (const String& srcUid, const String& dstUid) const
{
    bool anyAddable = false;
    for (int ch = 0; ch < 2; ++ch)
    {
        if (document.hasConnection (srcUid, ch, dstUid, ch))
            continue;   // that channel already exists — fine, nothing to add
        if (! document.canAddConnection (srcUid, ch, dstUid, ch))
            return false;
        anyAddable = true;
    }
    return anyAddable;   // false when the full pair already exists (pure duplicate)
}

bool GraphController::connectPair (const String& srcUid, const String& dstUid)
{
    if (! canConnectPair (srcUid, dstUid))
        return false;

    // Both channels validated above against the same topology (cycle checks are
    // node-level, so adding ch0 cannot invalidate ch1). One live rewire for the pair.
    for (int ch = 0; ch < 2; ++ch)
        if (! document.hasConnection (srcUid, ch, dstUid, ch))
            document.addConnection (srcUid, ch, dstUid, ch);

    rewireConnections();
    return true;
}

void GraphController::disconnectPair (const String& srcUid, const String& dstUid)
{
    // The canvas draws ONE cable for every document edge between this node pair, whatever
    // its channels, so removing the cable must remove all of them (a crossed or lone mono
    // edge from a legacy/hand-edited document would otherwise be an undeletable cable).
    struct Edge { int srcCh, dstCh; };
    std::vector<Edge> edges;
    const ValueTree connections = document.getConnections();
    for (int i = 0; i < connections.getNumChildren(); ++i)
    {
        const ValueTree c = connections.getChild (i);
        if (c.getProperty ("srcUid").toString() == srcUid && c.getProperty ("dstUid").toString() == dstUid)
            edges.push_back ({ (int) c.getProperty ("srcCh"), (int) c.getProperty ("dstCh") });
    }

    for (const auto& e : edges)
        document.removeConnection (srcUid, e.srcCh, dstUid, e.dstCh);
    rewireConnections();
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

    midiInputNode = addNodeBatched (std::make_unique<IOProcessor> (IOProcessor::midiInputNode));
    if (midiInputNode == nullptr)
        return;

    for (auto t : targets)
        connectBatched ({ { midiInputNode->nodeID, AudioProcessorGraph::midiChannelIndex },
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
