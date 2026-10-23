#include "GraphController.h"
#include "../PluginWindow.h"
#include "SettingsBackup.h"
#include "NodeGeometry.h"
#include "NodePlacement.h"
#include "Vst2State.h"

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
            migratedOnLoad = doc.migratedOnLoad;
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

    fixedOnLoad = repairOutputs();
    if (pinTargetNow.has_value())   // a device is already open: bind the unbound OUTs before the first build
        fixedOnLoad = document.pinLegacyOutputs (pinTargetNow->outCh, pinTargetNow->label, pinTargetNow->device) || fixedOnLoad;

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
    const std::set<String> side = document.getSideNodes();   // instruments beside the chain

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
        if (nodes.getChild (i).getProperty ("type").toString() == GraphDocument::typePlugin
            && side.count (nodes.getChild (i).getProperty ("uid").toString()) == 0)
            ++numPluginNodes;

    if (chain.back() != outUid || (int) chain.size() != numPluginNodes + 2)
    {
        chain.clear();
        chain.push_back (inUid);
        for (int i = 0; i < nodes.getNumChildren(); ++i)
        {
            ValueTree node = nodes.getChild (i);
            if (node.getProperty ("type").toString() == GraphDocument::typePlugin
                && side.count (node.getProperty ("uid").toString()) == 0)
                chain.push_back (node.getProperty ("uid").toString());
        }
        chain.push_back (outUid);
    }

    return chain;
}

void GraphController::rewriteChainConnections (const std::vector<String>& chainUids)
{
    // Edges of instruments beside the chain (source -> OUT) are not chain edges: keep them.
    const std::set<String> side = document.getSideNodes();
    std::vector<Connection> kept;
    for (const auto& c : getConnections())
        if (side.count (c.srcUid) > 0)
            kept.push_back (c);

    document.clearConnections();
    for (size_t i = 0; i + 1 < chainUids.size(); ++i)
    {
        document.addConnection (chainUids[i], 0, chainUids[i + 1], 0);
        document.addConnection (chainUids[i], 1, chainUids[i + 1], 1);
    }
    for (const auto& c : kept)
        document.addConnection (c.srcUid, c.srcCh, c.dstUid, c.dstCh);
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
    outputMeters.clear();
    nodeStrips.clear();
    nodeMixes.clear();
    midiInputNode = nullptr;
    monoNode = nullptr;
    missingPlugins.clear();

    ValueTree nodes = document.getNodes();
    for (int i = 0; i < nodes.getNumChildren(); ++i)
    {
        ValueTree nodeTree = nodes.getChild (i);
        const String uid  = nodeTree.getProperty ("uid").toString();
        const String type = nodeTree.getProperty ("type").toString();

        ignoreUnused (uid);
        if (type == GraphDocument::typePlugin)
            addPluginNodeToGraph (nodeTree);
        else
            registerIoNode (nodeTree);
    }

    connectDocumentEdges();

    insertNodeStrips();
    insertMasterMeters();
    insertInputConditioner();
    insertMidiRouting();
    publishTopology();
}

// An audioIn/audioOut document node -> its live graph IO node (other types are ignored).
void GraphController::registerIoNode (const ValueTree& nodeTree)
{
    using IOProcessor = AudioProcessorGraph::AudioGraphIOProcessor;
    const String type = nodeTree.getProperty ("type").toString();

    std::unique_ptr<AudioProcessor> processor;
    if (type == GraphDocument::typeAudioIn)
        processor = std::make_unique<IOProcessor> (IOProcessor::audioInputNode);
    else if (type == GraphDocument::typeAudioOut)
        processor = std::make_unique<IOProcessor> (IOProcessor::audioOutputNode);
    else
        return;

    if (auto node = addNodeBatched (std::move (processor)))
    {
        node->setBypassed ((bool) nodeTree.getProperty ("bypassed", false));
        uidToNodeId[nodeTree.getProperty ("uid").toString()] = node->nodeID;
    }
}

// Wire every document connection between live nodes. (An edge from the second channel of a
// mono synth is illegal here and skipped; spliceStripAfter re-creates it from the strip.)
void GraphController::connectDocumentEdges()
{
    ValueTree connections = document.getConnections();
    for (int i = 0; i < connections.getNumChildren(); ++i)
    {
        ValueTree c = connections.getChild (i);
        const auto src = uidToNodeId.find (c.getProperty ("srcUid").toString());
        const auto dst = uidToNodeId.find (c.getProperty ("dstUid").toString());
        if (src == uidToNodeId.end() || dst == uidToNodeId.end())
            continue;
        if (getOutputBase (c.getProperty ("dstUid").toString()) < 0)
            continue;   // an OUT whose pair isn't active stays unwired: nothing may alias onto another pair

        connectBatched ({ { src->second, (int) c.getProperty ("srcCh") },
                          { dst->second, (int) c.getProperty ("dstCh") } });
    }
}

// Instantiate one plugin document node into the live graph. On load failure the
// node becomes a PassThroughProcessor stand-in (so it does NOT sever the chain)
// and is recorded in missingPlugins; the saved plugin state is preserved because
// captureStates skips missing nodes.
AudioProcessorGraph::Node* GraphController::addPluginNodeToGraph (const ValueTree& nodeTree)
{
    return registerPluginNode (nodeTree, createPluginInstanceFor (nodeTree));
}

// Instantiate a plugin from its description and promote mono-defaulting plugins
// (e.g. MAutoPitch) to stereo before any state is restored, so they don't collapse
// a channel in the stereo chain. Touches nothing but the new instance.
std::unique_ptr<AudioProcessor> GraphController::createInstanceFrom (const PluginDescription& desc)
{
    std::unique_ptr<AudioProcessor> created;
    if (instanceFactory)
        created = instanceFactory (desc);
    else
    {
        String errorMessage;
        created = formatManager.createPluginInstance (desc, graph.getSampleRate(),
                                                      graph.getBlockSize(), errorMessage);
    }
    if (created == nullptr)
        return nullptr;

    lighthost::buses::forceStereo (*created, graph.getSampleRate(), graph.getBlockSize());
    return std::unique_ptr<AudioProcessor> (std::move (created));
}

// Create the hosted plugin for a document node (stereo-forced, saved state
// restored), or nullptr when it can't be loaded. Touches nothing but the new
// instance, so the retry path can call it before deciding to change the graph.
std::unique_ptr<AudioProcessor> GraphController::createPluginInstanceFor (const ValueTree& nodeTree)
{
    const auto desc = GraphDocument::getPluginDescription (nodeTree);
    if (! desc.has_value())
        return nullptr;

    auto created = createInstanceFrom (*desc);
    if (created == nullptr)
        return nullptr;

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

//==============================================================================
namespace
{
    std::vector<float> readParamValues (AudioProcessor& p)
    {
        std::vector<float> v;
        for (auto* param : p.getParameters())
            v.push_back (param->getValue());
        return v;
    }

    int countChanged (AudioProcessor& p, const std::vector<float>& baseline)
    {
        const auto& params = p.getParameters();
        int n = 0;
        for (int i = 0; i < params.size() && i < (int) baseline.size(); ++i)
            if (std::abs (params[i]->getValue() - baseline[(size_t) i]) > 1.0e-4f)
                ++n;
        return n;
    }

    // Feed `wrapped` (a VST3PluginState) to the fresh instance; if no parameter moved, put the
    // defaults back. A state-only change we can't see counts as failure.
    int tryWrappedState (AudioProcessor& p, const MemoryBlock& wrapped, const MemoryBlock& defaultState,
                         const std::vector<float>& defaults)
    {
        p.setStateInformation (wrapped.getData(), (int) wrapped.getSize());
        const int n = countChanged (p, defaults);
        if (n == 0)
            p.setStateInformation (defaultState.getData(), (int) defaultState.getSize());
        return n;
    }

    // Import a parsed VST2 state into a fresh (not yet graph-attached) VST3 instance.
    void importVst2State (AudioProcessor& p, const lighthost::vst2::Blob& blob, const MemoryBlock& whole,
                          GraphController::ReplaceResult& result)
    {
        using Import = GraphController::ReplaceResult::Import;
        namespace v2 = lighthost::vst2;

        MemoryBlock defaultState;
        p.getStateInformation (defaultState);
        const auto defaults = readParamValues (p);

        if (blob.kind == v2::Kind::bankChunk || blob.kind == v2::Kind::programChunk)
        {
            int n = tryWrappedState (p, v2::wrapAsVst3State (whole.getData(), whole.getSize()), defaultState, defaults);
            if (n > 0)
                result.import = Import::fullStream;
            else if ((n = tryWrappedState (p, v2::wrapAsVst3State (blob.chunk.getData(), blob.chunk.getSize()),
                                           defaultState, defaults)) > 0)
                result.import = Import::bareChunk;
            result.changedParams = n;
        }
        else
        {
            auto* bypass = p.getBypassParameter();
            const auto& params = p.getParameters();
            const int count = jmin ((int) blob.params.size(), params.size());
            for (int i = 0; i < count; ++i)
            {
                auto* param = params[i];
                if (param->isMetaParameter() || param == bypass)
                    continue;
                param->setValueNotifyingHost (jlimit (0.0f, 1.0f, blob.params[(size_t) i]));   // host side, like the param bridge
            }
            result.changedParams = countChanged (p, defaults);
            if (result.changedParams > 0)
                result.import = Import::paramsByIndex;
        }
    }
}

GraphController::ReplaceResult GraphController::replacePlugin (const String& uid, const PluginDescription& newDesc,
                                                               const MemoryBlock* fallbackState)
{
    ReplaceResult result;

    ValueTree nodeTree = document.getNodeByUid (uid);
    if (! nodeTree.isValid() || nodeTree.getProperty ("type").toString() != GraphDocument::typePlugin)
    {
        result.message = "Not a plugin node";
        return result;
    }

    // Instantiate FIRST: a plugin that can't load leaves the document and the graph untouched.
    auto instance = createInstanceFrom (newDesc);
    if (instance == nullptr)
    {
        result.message = "Could not load " + newDesc.name;
        return result;
    }

    // Source state: a live plugin's current state, else the saved one (stand-ins never hold it),
    // else the caller's fallback (the legacy settings key).
    MemoryBlock source;
    if (auto* live = isMissing (uid) ? nullptr : getNodeForUid (uid))
        live->getProcessor()->getStateInformation (source);
    else
        source.fromBase64Encoding (nodeTree.getProperty ("state").toString());
    if (source.isEmpty() && fallbackState != nullptr)
        source = *fallbackState;

    std::optional<lighthost::vst2::Blob> vst2;
    if (newDesc.pluginFormatName == "VST3" && ! source.isEmpty())
        vst2 = lighthost::vst2::parse (source.getData(), source.getSize());

    if (vst2.has_value())
    {
        importVst2State (*instance, *vst2, source, result);
        result.message = result.import == ReplaceResult::Import::paramsByIndex
                             ? String (CharPointer_UTF8 ("Parameters copied by position \xe2\x80\x94 check by ear"))
                         : result.import != ReplaceResult::Import::none
                             ? String ("Settings imported")
                             : String (CharPointer_UTF8 ("Defaults loaded \xe2\x80\x94 the original settings stay saved with the preset"));
    }
    else
    {
        result.message = source.isEmpty() ? "Plugin replaced" : "Plugin replaced, the new plugin starts at its defaults";
    }

    MemoryBlock newState;
    instance->getStateInformation (newState);
    if (! document.replacePlugin (uid, newDesc, newState.toBase64Encoding(), source.toBase64Encoding()))
    {
        result = {};
        result.message = "Not a plugin node";
        return result;
    }

    // An instrument has no input: edges that fed the old plugin can't survive the swap.
    if (GraphDocument::isSourceDescription (newDesc))
        for (const auto& c : getConnections())
            if (c.dstUid == uid)
                document.removeConnection (c.srcUid, c.srcCh, c.dstUid, c.dstCh);

    // Same swap as the missing-plugin recovery; removal also closes the node's editor window.
    removePluginNodeFromGraph (uid);
    const bool registered = registerPluginNode (nodeTree, std::move (instance)) != nullptr;
    jassert (registered);
    rewireConnections();   // the one publish for this gesture
    result.ok = registered;
    return result;
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

// Adds the document node between the last card and OUT. Existing cards never move; OUT is pushed right when
// it is in the way. Positions are GUI-only, so this publishes nothing.
String GraphController::addPluginNodeAtNextSlot (const PluginDescription& desc)
{
    const bool source = GraphDocument::isSourceDescription (desc);

    String uid;
    if (source)
    {
        const auto at = lighthost::placeNewSource (document);
        uid = document.addPluginNode (desc, at.x, at.y);
    }
    else
    {
        const auto placement = lighthost::placeNewPlugin (document);
        uid = document.addPluginNode (desc, placement.centre.x, placement.centre.y);
        if (placement.moveOutTo.has_value())
            document.setNodePosition (document.getIoNodeUid (false), placement.moveOutTo->x, placement.moveOutTo->y);
    }

    document.setReceivesMidi (uid, source);
    return uid;
}

String GraphController::addSourcePlugin (const PluginDescription& desc)
{
    const String uid = addPluginNodeAtNextSlot (desc);
    const String out = document.getIoNodeUid (false);
    document.addConnection (uid, 0, out, 0);
    document.addConnection (uid, 1, out, 1);

    addPluginNodeToGraph (document.getNodeByUid (uid));
    rewireConnections();   // the one publish for this gesture
    return uidToNodeId.count (uid) > 0 ? uid : String();
}

String GraphController::addPlugin (const PluginDescription& desc)
{
    if (GraphDocument::isSourceDescription (desc))
        return addSourcePlugin (desc);

    // Serial append rewrites ALL connections as a chain, which would destroy
    // parallel routing — on a branched graph the node is added unconnected instead.
    return isLinearChain() ? appendToChain (desc) : addNodeUnconnected (desc);
}

String GraphController::appendToChain (const PluginDescription& desc)
{
    if (GraphDocument::isSourceDescription (desc))
        return addSourcePlugin (desc);   // an instrument has no place in a serial effect chain

    std::vector<String> chain = getChainUids();

    const String uid = addPluginNodeAtNextSlot (desc);
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
    if (GraphDocument::isSourceDescription (desc))
        return addSourcePlugin (desc);

    const String uid = addPluginNodeAtNextSlot (desc);
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
    if (uid == document.getIoNodeUid (true) || document.isMainOutput (uid))
        return;   // IN and the main OUT are permanent

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
    outputMeters.clear();

    const auto itIn = uidToNodeId.find (document.getIoNodeUid (true));
    const String mainUid = document.getIoNodeUid (false);

    // One guard + meter in front of every OUT that is wired: measures exactly what reaches that pair.
    // The main OUT always gets them (as before v1.2); an extra OUT only while something feeds it, so an
    // unused OUT costs nothing. An OUT whose pair is inactive is left unwired and gets neither.
    for (const auto& outUid : document.getOutputUids())
    {
        const auto itOut = uidToNodeId.find (outUid);
        const int base = getOutputBase (outUid);
        if (itOut == uidToNodeId.end() || base < 0)
            continue;

        const bool isMain = outUid == mainUid;
        if (! isMain)
        {
            bool fed = false;
            for (const auto& c : graph.getConnections())
                fed = fed || c.destination.nodeID == itOut->second;
            if (! fed)
                continue;
        }

        auto* meter = spliceMeterBefore (itOut->second, base);
        if (meter == nullptr)
        {
            // Can't happen short of an allocation failure; but this OUT's edges are still on its relative
            // channels 0/1 and would alias onto the first pair, so cut them: silent beats misrouted.
            jassertfalse;
            for (const auto& c : graph.getConnections())
                if (c.destination.nodeID == itOut->second)
                    disconnectBatched (c);
            continue;
        }

        outputMeters[outUid] = meter;
        if (isMain)
            outputMeter = meter;

        // Safety limiter goes in front of the output meter, so the meter shows what
        // actually reaches the device: ... -> guard -> meter -> audioOut.
        for (auto* n : graph.getNodes())
            if (n->getProcessor() == meter)
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
AudioProcessorGraph::Node* GraphController::spliceProcBefore (AudioProcessorGraph::NodeID target, int targetBaseChannel)
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
        connectBatched ({ { nodeId, ch }, { target, targetBaseChannel + ch } });

    return node.get();
}

MeterProcessor* GraphController::spliceMeterBefore (AudioProcessorGraph::NodeID target, int targetBaseChannel)
{
    auto* node = spliceProcBefore<MeterProcessor> (target, targetBaseChannel);
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

const MeterTap* GraphController::getOutputMeter (const String& outUid) const noexcept
{
    const auto it = outputMeters.find (outUid);
    return it != outputMeters.end() && it->second != nullptr ? &it->second->getTap() : nullptr;
}

//==============================================================================
// Output nodes bound to device channel pairs (v1.2 B).

int GraphController::getOutputBase (const String& outUid) const
{
    if (document.getNodeByUid (outUid).getProperty ("type").toString() != GraphDocument::typeAudioOut)
        return 0;

    lighthost::outputs::Binding b;
    b.outCh      = document.getOutputChannel (outUid);
    b.bound      = b.outCh >= 0;
    b.outDevice  = document.getOutputDevice (outUid);
    b.isMain     = document.isMainOutput (outUid);
    return lighthost::outputs::outputBase (b, outputLayout);
}

std::map<String, int> GraphController::outputBases() const
{
    std::map<String, int> bases;
    for (const auto& uid : document.getOutputUids())
        bases[uid] = getOutputBase (uid);
    return bases;
}

void GraphController::rewireIfOutputsMoved (const std::map<String, int>& before)
{
    if (! uidToNodeId.empty() && outputBases() != before)
        rewireConnections();   // one publish; plugins untouched
}

bool GraphController::setOutputLayout (const lighthost::outputs::Layout& layout)
{
    if (layout == outputLayout)
        return false;

    const auto before = outputBases();
    outputLayout = layout;
    pinTargetNow = lighthost::outputs::pinTarget (layout);
    rewireIfOutputsMoved (before);
    return true;
}

bool GraphController::pinLegacyOutputs (const lighthost::outputs::PinTarget& target)
{
    const auto before = outputBases();
    const bool changed = document.pinLegacyOutputs (target.outCh, target.label, target.device);
    if (changed)
        rewireIfOutputsMoved (before);
    return changed;
}

bool GraphController::repairOutputs()
{
    if (! outputLayout.known || outputLayout.names.isEmpty())
        return false;
    return document.removeInvalidOutputs (outputLayout.deviceName, outputLayout.names.size());
}

String GraphController::addOutput (int outCh)
{
    if (! outputLayout.known || outCh < 0 || outCh + 1 >= outputLayout.names.size())
        return {};   // the pair must exist on the open device

    // Below the lowest OUT already on the canvas.
    const String mainUid = document.getIoNodeUid (false);
    float x = document.getNodeX (mainUid), y = document.getNodeY (mainUid);
    for (const auto& uid : document.getOutputUids())
        y = jmax (y, document.getNodeY (uid));
    y += (float) (lighthost::geometry::endpointHeight + lighthost::geometry::nodeGap);

    const String uid = document.addOutputNode (outCh, lighthost::outputs::pairLabel (outputLayout.names, outCh),
                                               outputLayout.deviceName, x, y);
    registerIoNode (document.getNodeByUid (uid));
    rewireConnections();   // the one publish for this gesture
    return uid;
}

void GraphController::setOutputBinding (const String& outUid, int outCh)
{
    if (! outputLayout.known || outCh < 0 || outCh + 1 >= outputLayout.names.size()
        || (document.getOutputChannel (outUid) == outCh && document.getOutputDevice (outUid) == outputLayout.deviceName))
        return;
    if (document.getNodeByUid (outUid).getProperty ("type").toString() != GraphDocument::typeAudioOut)
        return;

    document.setOutputBinding (outUid, outCh, lighthost::outputs::pairLabel (outputLayout.names, outCh), outputLayout.deviceName);
    rewireConnections();   // the one publish for this gesture
}

String GraphController::getOutputDisplayLabel (const String& outUid) const
{
    const String stored = document.getOutputLabel (outUid);
    if (stored.isNotEmpty())
        return stored;
    return pinTargetNow.has_value() ? pinTargetNow->label : String();
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

        auto* stripNode = spliceStripAfter (it->second, document.getGain (uid), document.getPan (uid),
                                            document.isSourceNode (uid) ? uid : String());
        if (stripNode == nullptr)
            continue;

        if (auto* strip = dynamic_cast<NodeStripProcessor*> (stripNode->getProcessor()))
            nodeStrips[uid] = strip;

        // Dry/wet: the parallel dry branch exists only while the node is not fully
        // wet, so a default graph (every mix == 1) keeps the plain topology.
        const float mix = document.getMix (uid);   // always 1 for an instrument
        if (mix < 1.0f)
            insertMixNode (it->second, stripNode->nodeID, uid, mix);
    }
}

// source -> strip -> (old destinations of source). Mirrors spliceMeterAfter but
// carries the node's gain/pan. Returns the live strip node (for the mix splice).
AudioProcessorGraph::Node* GraphController::spliceStripAfter (AudioProcessorGraph::NodeID source, float gain, float pan,
                                                              const String& monoSourceUid)
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

    // A synth that only offers one output channel feeds both strip channels (no extra node), and its
    // cable's second channel (illegal on the synth itself) leaves from the strip's channel 1 instead.
    // An effect that stayed mono keeps today's behaviour.
    if (monoSourceUid.isNotEmpty())
        if (auto* n = graph.getNodeForId (source))
            if (n->getProcessor()->getTotalNumOutputChannels() == 1)
            {
                connectBatched ({ { source, 0 }, { stripId, 1 } });

                ValueTree docEdges = document.getConnections();
                for (int i = 0; i < docEdges.getNumChildren(); ++i)
                {
                    ValueTree e = docEdges.getChild (i);
                    if (e.getProperty ("srcUid").toString() != monoSourceUid || (int) e.getProperty ("srcCh") != 1)
                        continue;
                    const auto dst = uidToNodeId.find (e.getProperty ("dstUid").toString());
                    if (dst != uidToNodeId.end())
                        connectBatched ({ { stripId, 1 }, { dst->second, (int) e.getProperty ("dstCh") } });
                }
            }

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
    if (document.isSourceNode (uid))
        return;   // an instrument has no dry signal

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

    connectDocumentEdges();

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
    GraphDocument::migrateTree (document.state);   // safety net: a v1 tree must never reach the canvas
    repairOutputs();
    if (pinTargetNow.has_value())
        document.pinLegacyOutputs (pinTargetNow->outCh, pinTargetNow->label, pinTargetNow->device);
    rebuild();
}

GraphDocument GraphController::snapshotDocument()
{
    captureStates();                           // fold live plugin states into the document
    GraphDocument g;
    g.state = document.state.createCopy();
    return g;
}
