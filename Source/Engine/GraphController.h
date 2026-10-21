//
//  GraphController.h — owns the live AudioProcessorGraph and keeps it in sync
//  with a GraphDocument. All methods must be called from the message thread;
//  AudioProcessorGraph handles the realtime-safe swap internally.
//
//  Milestone 2 exposes serial-chain operations (what the tray menu needs).
//  The document and connection APIs already support arbitrary graphs — the
//  node-canvas GUI will drive those directly in a later milestone.
//

#pragma once

#include <JuceHeader.h>
#include <map>
#include <vector>
#include <set>
#include "GraphDocument.h"
#include "MeterProcessor.h"
#include "NodeStripProcessor.h"
#include "PluginParameters.h"
#include "PassThroughProcessor.h"
#include "PluginBuses.h"
#include "MonoInputProcessor.h"
#include "MixProcessor.h"
#include "OutputGuardProcessor.h"

class GraphController
{
public:
    struct ChainItem
    {
        String uid;
        String name;
        bool bypassed = false;
        bool missing = false;    // plugin failed to load (uninstalled/broken)
    };

    GraphController (AudioProcessorGraph&, AudioPluginFormatManager&);

    //==============================================================================
    void loadFrom (PropertiesFile&);   // reads "graphDocument", migrates legacy settings if absent
    void save (PropertiesFile&);       // captures live plugin states, persists XML
    // Non-empty when the last loadFrom() found an unreadable saved graph: a copy was kept
    // (see SettingsBackup.h) and the controller started from defaults.
    String getLoadWarning() const { return loadWarning; }

    //==============================================================================
    // Serial-chain operations (tray menu)
    std::vector<ChainItem> getChain() const;
    // The one add entry point for UI code: appends serially while the graph is a
    // linear chain, otherwise adds the node unconnected (the user wires it on the
    // canvas). Keeps the linearize-on-add decision out of the call sites.
    String addPlugin (const PluginDescription&);       // returns new node uid, "" on failure
    String appendToChain (const PluginDescription&);   // serial add (linear chains)
    String addNodeUnconnected (const PluginDescription&);   // no connections touched
    // Remove ONE node and heal around it (bridge each upstream edge to each
    // downstream edge with channel continuity). No other edge is touched and no
    // plugin is reloaded, so branched routing and live instances survive a delete.
    void removeNode (const String& uid);
    void moveUp (const String& uid);
    void moveDown (const String& uid);
    void setBypassed (const String& uid, bool);        // live pass-through, no rebuild
    void clearAllPluginStates();                       // resets plugins to defaults

    //==============================================================================
    AudioProcessorGraph::Node* getNodeForUid (const String& uid) const;
    bool isMissing (const String& uid) const noexcept { return missingPlugins.count (uid) > 0; }
    // No usable plugin behind this node: a pass-through stand-in OR never made it into the
    // live graph at all (e.g. an unknown type in a newer/corrupt document). The one predicate
    // the cards, the canvas badge and the tray use.
    bool isNodeUnavailable (const String& uid) const { return isMissing (uid) || getNodeForUid (uid) == nullptr; }

    // Missing plugins (round-3 finding 6, option c). A plugin that failed to load
    // runs as a PassThrough stand-in until it is EXPLICITLY retried: edits never
    // re-instantiate it (a synchronous load on every gesture could block the UI for
    // seconds). On success the stand-in becomes the real plugin under the same uid
    // (saved state + bypass restored, wiring re-derived from the document, other
    // instances untouched) with ONE publish; on failure nothing changes or publishes.
    int  getNumMissingPlugins() const noexcept { return (int) missingPlugins.size(); }
    bool retryMissingPlugin (const String& uid);   // true when the plugin is live again
    int  retryMissingPlugins();                     // number recovered; one publish for all

    //==============================================================================
    // Master meter taps for the GUI (message-thread reads). Null until the first
    // rebuild, or if the corresponding IO node is absent. Meters are spliced into
    // the live graph only — they are never stored in the GraphDocument.
    const MeterTap* getInputMeter()  const noexcept;
    const MeterTap* getOutputMeter() const noexcept;

    //==============================================================================
    // Per-node channel strip (M4): output gain + stereo balance, plus the node's
    // own post-strip meter. Gain/pan persist in the document; setters apply live
    // (no rebuild). getNodeMeter is null for a missing plugin or before load.
    void  setNodeGain (const String& uid, float gain);
    float getNodeGain (const String& uid) const;
    void  setNodePan  (const String& uid, float pan);
    float getNodePan  (const String& uid) const;
    const MeterTap* getNodeMeter (const String& uid) const noexcept;

    // Per-node dry/wet mix: 0 = fully dry (plugin bypassed in the blend), 1 = fully
    // wet (default; the plain topology). Persists in the document. The parallel dry
    // branch exists only while mix < 1: crossing 100 % in either direction builds or
    // tears it down with ONE rewire (one publish, no plugin reload); changes below
    // 100 % apply live via the atomic and publish nothing.
    void  setNodeMix (const String& uid, float mix);
    float getNodeMix (const String& uid) const;

    //==============================================================================
    // Arbitrary routing (M6). connect/disconnect validate + edit the document, then
    // re-wire the LIVE graph WITHOUT reloading plugins. Node positions are GUI-only.
    struct Connection { String srcUid; int srcCh; String dstUid; int dstCh; };
    bool connect    (const String& srcUid, int srcCh, const String& dstUid, int dstCh);
    void disconnect (const String& srcUid, int srcCh, const String& dstUid, int dstCh);
    bool canConnect (const String& srcUid, int srcCh, const String& dstUid, int dstCh) const;
    std::vector<Connection> getConnections() const;

    // A canvas cable is a stereo pair (ch0+ch1, straight). The pair calls treat the
    // full pair as the goal state: connectPair adds whichever channels are missing
    // (so it can complete a half pair from a legacy/hand-edited document) with ONE
    // live rewire; canConnectPair is false only if a channel is neither present nor
    // addable, or the pair already exists in full.
    bool canConnectPair (const String& srcUid, const String& dstUid) const;
    bool connectPair    (const String& srcUid, const String& dstUid);
    void disconnectPair (const String& srcUid, const String& dstUid);

    // Presets: apply a stored graph document (deep copy + rebuild), or snapshot
    // the live one (capturing plugin states) for saving into a preset.
    void loadDocument (const GraphDocument&);
    GraphDocument snapshotDocument();
    bool isLinearChain() const;   // tray serial ops are only offered when true

    // Mono input: sum the audio input to mono and feed both output channels, so a
    // mono source that arrives on one channel (e.g. a mic on the right input) is
    // heard on both. App-global setting; live re-splice, no plugin reload.
    void setMonoInput (bool);
    bool isMonoInput() const noexcept { return monoInput; }

    // MIDI routing (M7): host MIDI reaches plugins that acceptsMidi() and whose
    // per-node receivesMidi flag is set. Live re-splice, no plugin reload.
    void setNodeReceivesMidi (const String& uid, bool);
    bool getNodeReceivesMidi (const String& uid) const;
    bool nodeAcceptsMidi (const String& uid) const;   // queries the live plugin
    void  setNodePosition (const String& uid, float x, float y);
    float getNodeX (const String& uid) const;
    float getNodeY (const String& uid) const;

    //==============================================================================
    // Plugin parameter bridge (M5): read/drive the hosted plugin's own parameters
    // (Freq, Gain, Ratio, …). Empty / no-op for a missing plugin. Message thread.
    std::vector<lighthost::params::ParamInfo> getNodeParameters (const String& uid, bool automatableOnly = false) const;
    std::vector<lighthost::params::ParamInfo> getNodeCardParameters (const String& uid, int count) const;
    void  setNodeParameter (const String& uid, int index, float normalized);
    float getNodeParameter (const String& uid, int index) const;
    String getNodeParameterText (const String& uid, int index) const;   // plugin's own value text

    // Number of render-sequence publishes so far; a user gesture must raise it by exactly 1 (Test P).
    int getPublishCount() const noexcept { return publishCount; }

    GraphDocument document;

private:
    // Batched graph mutation: the ONLY callers of graph.addNode/removeNode/addConnection/
    // removeConnection/clear. They use UpdateKind::none; publishTopology() sends the result
    // to the audio thread once per gesture.
    AudioProcessorGraph::Node::Ptr addNodeBatched (std::unique_ptr<AudioProcessor>);
    AudioProcessorGraph::Node::Ptr removeNodeBatched (AudioProcessorGraph::NodeID);
    bool connectBatched (const AudioProcessorGraph::Connection&);
    bool disconnectBatched (const AudioProcessorGraph::Connection&);
    void clearBatched();
    void publishTopology();
    int publishCount = 0;

    void rebuild();                          // document -> live graph (reentrancy-safe wrapper)
    void rebuildNow();                       // the actual teardown/build pass
    void rewireConnections();                // re-wire live graph from doc WITHOUT reloading plugins
    void captureStates();                    // live plugin states -> document
    std::vector<String> getChainUids() const;            // audioIn .. audioOut, doc order
    void rewriteChainConnections (const std::vector<String>& chainUids);
    juce::Point<float> placementForNewNode() const;   // right of the rightmost plugin, never stacked
    // Instantiate ONE plugin document node into the live graph (PassThrough stand-in
    // + missingPlugins entry when the plugin can't load) and register it in
    // uidToNodeId. Shared by rebuildNow and the incremental add paths.
    AudioProcessorGraph::Node* addPluginNodeToGraph (const ValueTree& nodeTree);
    std::unique_ptr<AudioProcessor> createPluginInstanceFor (const ValueTree& nodeTree);   // nullptr if it can't load
    AudioProcessorGraph::Node* registerPluginNode (const ValueTree& nodeTree, std::unique_ptr<AudioProcessor> instance);
    // Teardown mirror of addPluginNodeToGraph: every per-uid runtime map is cleaned here.
    void removePluginNodeFromGraph (const String& uid);
    bool swapInRecoveredPlugin (const String& uid);   // stand-in -> real plugin; no publish

    // Meter plumbing (runtime only).
    void insertMasterMeters();
    MeterProcessor* spliceMeterBefore (AudioProcessorGraph::NodeID target);
    OutputGuardProcessor* spliceGuardBefore (AudioProcessorGraph::NodeID target);
    // Shared body: redirect every X -> target connection through a new Proc node.
    template <typename Proc>
    AudioProcessorGraph::Node* spliceProcBefore (AudioProcessorGraph::NodeID target);
    MeterProcessor* spliceMeterAfter  (AudioProcessorGraph::NodeID source);

    void insertNodeStrips();
    void insertMidiRouting();
    AudioProcessorGraph::Node* spliceStripAfter (AudioProcessorGraph::NodeID source, float gain, float pan);
    // Splice a dry/wet mix node between a plugin and its strip: plugin -> mix(wet) -> strip,
    // with the plugin's own input sources tapped in parallel into mix's dry inputs.
    void insertMixNode (AudioProcessorGraph::NodeID pluginId, AudioProcessorGraph::NodeID stripId,
                        const String& uid, float mix);

    void insertInputConditioner();   // splice the mono-collapse node when monoInput is on
    MonoInputProcessor* spliceMonoAfter (AudioProcessorGraph::NodeID source);

    AudioProcessorGraph& graph;
    AudioPluginFormatManager& formatManager;
    std::map<String, AudioProcessorGraph::NodeID> uidToNodeId;
    std::set<String> missingPlugins;   // plugin nodes running as pass-through (failed to load)
    MeterProcessor* inputMeter  = nullptr;   // owned by the graph node, not us
    MeterProcessor* outputMeter = nullptr;
    std::map<String, NodeStripProcessor*> nodeStrips;   // plugin uid -> its strip (graph-owned)
    std::map<String, MixProcessor*> nodeMixes;          // plugin uid -> its dry/wet node (graph-owned), only while mix < 1
    AudioProcessorGraph::Node::Ptr midiInputNode;       // runtime MIDI source (graph-owned)
    bool monoInput = false;                             // sum input to mono, feed both channels
    MonoInputProcessor* monoNode = nullptr;             // runtime mono-collapse node (graph-owned)
    // Reentrant-rebuild coalescing — see the comment on rebuild() in the .cpp.
    String loadWarning;
    bool rebuilding = false;
    bool rebuildPending = false;

    JUCE_DECLARE_NON_COPYABLE (GraphController)
};
