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

    // Keep default canvas positions tidy: spread plugins between the IO nodes.
    const auto pluginCount = chainUids.size() > 2 ? chainUids.size() - 2 : 0;
    for (size_t i = 1; i + 1 < chainUids.size(); ++i)
    {
        ValueTree node = document.getNodeByUid (chainUids[i]);
        node.setProperty ("x", 0.1f + 0.8f * (float) i / (float) (pluginCount + 1), nullptr);
        node.setProperty ("y", 0.5f, nullptr);
    }
}

//==============================================================================
void GraphController::rebuild()
{
    PluginWindow::closeAllCurrentlyOpenWindows();
    graph.clear();
    uidToNodeId.clear();
    inputMeter = outputMeter = nullptr;

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
            if (auto desc = GraphDocument::getPluginDescription (nodeTree))
            {
                String errorMessage;
                auto instance = formatManager.createPluginInstance (*desc, graph.getSampleRate(),
                                                                    graph.getBlockSize(), errorMessage);
                if (instance != nullptr)
                {
                    const String stateB64 = nodeTree.getProperty ("state").toString();
                    MemoryBlock stateBinary;
                    if (stateBinary.fromBase64Encoding (stateB64) && stateBinary.getSize() > 0)
                        instance->setStateInformation (stateBinary.getData(), (int) stateBinary.getSize());
                    processor = std::move (instance);
                }
                // nullptr: plugin missing/broken — node stays in the document,
                // has no live counterpart, and its connections are skipped below.
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

    insertMasterMeters();
}

void GraphController::captureStates()
{
    ValueTree nodes = document.getNodes();
    for (int i = 0; i < nodes.getNumChildren(); ++i)
    {
        ValueTree nodeTree = nodes.getChild (i);
        if (nodeTree.getProperty ("type").toString() != GraphDocument::typePlugin)
            continue;

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
        item.missing  = uidToNodeId.find (item.uid) == uidToNodeId.end();
        result.push_back (item);
    }
    return result;
}

String GraphController::appendToChain (const PluginDescription& desc)
{
    captureStates();
    std::vector<String> chain = getChainUids();
    const String uid = document.addPluginNode (desc, 0.5f, 0.5f);
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

const MeterTap* GraphController::getInputMeter() const noexcept
{
    return inputMeter != nullptr ? &inputMeter->getTap() : nullptr;
}

const MeterTap* GraphController::getOutputMeter() const noexcept
{
    return outputMeter != nullptr ? &outputMeter->getTap() : nullptr;
}
