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

    GraphDocument document;

private:
    void rebuild();                          // document -> live graph
    void captureStates();                    // live plugin states -> document
    std::vector<String> getChainUids() const;            // audioIn .. audioOut, doc order
    void rewriteChainConnections (const std::vector<String>& chainUids);

    // Meter plumbing (runtime only).
    void insertMasterMeters();
    MeterProcessor* spliceMeterBefore (AudioProcessorGraph::NodeID target);
    MeterProcessor* spliceMeterAfter  (AudioProcessorGraph::NodeID source);

    AudioProcessorGraph& graph;
    AudioPluginFormatManager& formatManager;
    std::map<String, AudioProcessorGraph::NodeID> uidToNodeId;
    MeterProcessor* inputMeter  = nullptr;   // owned by the graph node, not us
    MeterProcessor* outputMeter = nullptr;

    JUCE_DECLARE_NON_COPYABLE (GraphController)
};
