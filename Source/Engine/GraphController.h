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

    GraphDocument document;

private:
    void rebuild();                          // document -> live graph
    void captureStates();                    // live plugin states -> document
    std::vector<String> getChainUids() const;            // audioIn .. audioOut, doc order
    void rewriteChainConnections (const std::vector<String>& chainUids);

    AudioProcessorGraph& graph;
    AudioPluginFormatManager& formatManager;
    std::map<String, AudioProcessorGraph::NodeID> uidToNodeId;

    JUCE_DECLARE_NON_COPYABLE (GraphController)
};
