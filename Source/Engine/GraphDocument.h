//
//  GraphDocument.h — persistent description of the signal graph.
//
//  A thin wrapper around a ValueTree:
//
//  <GRAPH version="1">
//    <NODES>
//      <NODE uid="…" type="audioIn"|"audioOut"|"plugin" name="…" x="0.5" y="0.5"
//            bypassed="0" state="base64…">        (state/PLUGIN only for type="plugin")
//        <PLUGIN …/>                              (PluginDescription XML)
//      </NODE>
//    </NODES>
//    <CONNECTIONS>
//      <CONNECTION srcUid="…" srcCh="0" dstUid="…" dstCh="0"/>
//    </CONNECTIONS>
//  </GRAPH>
//
//  Nodes carry stable UUIDs, so two instances of the same plugin never collide
//  (the legacy PropertiesFile keyed by name+version+format could not do this).
//

#pragma once

#include <JuceHeader.h>
#include <optional>

class GraphDocument
{
public:
    GraphDocument();                                   // empty graph with audioIn/audioOut nodes

    static GraphDocument fromXml (const String& xml);  // check isValid() on the result
    String toXml() const;
    bool isValid() const;

    // Node type strings
    static const char* const typeAudioIn;
    static const char* const typeAudioOut;
    static const char* const typePlugin;

    //==============================================================================
    String addPluginNode (const PluginDescription&, float x, float y);
    void removeNode (const String& uid);               // also drops its connections
    ValueTree getNodeByUid (const String& uid) const;
    ValueTree getNodes() const;
    ValueTree getConnections() const;
    String getIoNodeUid (bool input) const;

    void setBypassed (const String& uid, bool);
    bool isBypassed (const String& uid) const;

    void setPluginState (const String& uid, const String& base64);
    String getPluginState (const String& uid) const;
    void clearAllPluginStates();

    static std::optional<PluginDescription> getPluginDescription (const ValueTree& node);

    //==============================================================================
    void addConnection (const String& srcUid, int srcCh, const String& dstUid, int dstCh);
    void clearConnections();
    void removeConnectionsInvolving (const String& uid);

    //==============================================================================
    // Builds a document from the pre-2026 LightHost settings keys
    // (pluginListActive + plugin-order-*/plugin-bypass-*/plugin-state-*).
    // Returns an empty default document if there is nothing to migrate.
    static GraphDocument migrateFromLegacySettings (PropertiesFile&);

    ValueTree state;   // the GRAPH tree; public for future GUI/undo binding

private:
    String addNodeInternal (const String& type, const String& name, float x, float y);
};
