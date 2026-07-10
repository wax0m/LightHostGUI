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
#include "GraphDocument.h"
#include "MeterProcessor.h"
#include "NodeStripProcessor.h"
#include "PluginParameters.h"

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

    //==============================================================================
    // Serial-chain operations (tray menu)
    std::vector<ChainItem> getChain() const;
    String appendToChain (const PluginDescription&);   // returns new node uid, "" on failure
    void removeFromChain (const String& uid);
    void moveUp (const String& uid);
    void moveDown (const String& uid);
    void setBypassed (const String& uid, bool);        // live pass-through, no rebuild
    void clearAllPluginStates();                       // resets plugins to defaults

    //==============================================================================
    AudioProcessorGraph::Node* getNodeForUid (const String& uid) const;

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

    //==============================================================================
    // Arbitrary routing (M6). connect/disconnect validate + edit the document, then
    // re-wire the LIVE graph WITHOUT reloading plugins. Node positions are GUI-only.
    struct Connection { String srcUid; int srcCh; String dstUid; int dstCh; };
    bool connect    (const String& srcUid, int srcCh, const String& dstUid, int dstCh);
    void disconnect (const String& srcUid, int srcCh, const String& dstUid, int dstCh);
    bool canConnect (const String& srcUid, int srcCh, const String& dstUid, int dstCh) const;
    std::vector<Connection> getConnections() const;

    // Presets: apply a stored graph document (deep copy + rebuild), or snapshot
    // the live one (capturing plugin states) for saving into a preset.
    void loadDocument (const GraphDocument&);
    GraphDocument snapshotDocument();
    bool isLinearChain() const;   // tray serial ops are only offered when true

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

    GraphDocument document;

private:
    void rebuild();                          // document -> live graph
    void rewireConnections();                // re-wire live graph from doc WITHOUT reloading plugins
    void captureStates();                    // live plugin states -> document
    std::vector<String> getChainUids() const;            // audioIn .. audioOut, doc order
    void rewriteChainConnections (const std::vector<String>& chainUids);

    // Meter plumbing (runtime only).
    void insertMasterMeters();
    MeterProcessor* spliceMeterBefore (AudioProcessorGraph::NodeID target);
    MeterProcessor* spliceMeterAfter  (AudioProcessorGraph::NodeID source);

    void insertNodeStrips();
    void insertMidiRouting();
    NodeStripProcessor* spliceStripAfter (AudioProcessorGraph::NodeID source, float gain, float pan);

    AudioProcessorGraph& graph;
    AudioPluginFormatManager& formatManager;
    std::map<String, AudioProcessorGraph::NodeID> uidToNodeId;
    MeterProcessor* inputMeter  = nullptr;   // owned by the graph node, not us
    MeterProcessor* outputMeter = nullptr;
    std::map<String, NodeStripProcessor*> nodeStrips;   // plugin uid -> its strip (graph-owned)
    AudioProcessorGraph::Node::Ptr midiInputNode;       // runtime MIDI source (graph-owned)

    JUCE_DECLARE_NON_COPYABLE (GraphController)
};
