//
//  GraphDocument.h — persistent description of the signal graph.
//
//  A thin wrapper around a ValueTree:
//
//  <GRAPH version="2">
//    <NODES>
//      <NODE uid="…" type="audioIn"|"audioOut"|"plugin" name="…" x="400" y="260"
//            bypassed="0" state="base64…">        (state/PLUGIN only for type="plugin")
//        <PLUGIN …/>                              (PluginDescription XML)
//      </NODE>
//    </NODES>
//    <CONNECTIONS>
//      <CONNECTION srcUid="…" srcCh="0" dstUid="…" dstCh="0"/>
//    </CONNECTIONS>
//  </GRAPH>
//
//  x / y are the node's CENTRE in logical canvas px at 100 % zoom (version 2; version 1 stored them
//  normalised 0..1 and is migrated on load by migrateTree).
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

    static constexpr int currentVersion = 2;

    static GraphDocument fromXml (const String& xml);  // check isValid() on the result; migrates v1 -> v2
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

    // Non-finite -> fallback, else clamped. Applied in getters AND setters so a corrupt or
    // hand-edited settings file can't push out-of-range values into the engine.
    static float sanitise (float v, float lo, float hi, float fallback);

    void  setGain (const String& uid, float);
    float getGain (const String& uid) const;   // linear, default 1.0 (0 dB)
    void  setPan  (const String& uid, float);
    float getPan  (const String& uid) const;    // -1..+1 balance, default 0
    void  setReceivesMidi (const String& uid, bool);
    bool  getReceivesMidi (const String& uid) const;   // default true; gates host MIDI to this node
    void  setMix  (const String& uid, float);
    float getMix  (const String& uid) const;   // dry/wet, 0=dry 1=wet, default 1.0 (fully wet)
    void setPluginState (const String& uid, const String& base64);
    String getPluginState (const String& uid) const;
    void clearAllPluginStates();

    static std::optional<PluginDescription> getPluginDescription (const ValueTree& node);

    //==============================================================================
    void addConnection (const String& srcUid, int srcCh, const String& dstUid, int dstCh);
    void clearConnections();
    void removeConnectionsInvolving (const String& uid);

    //==============================================================================
    // Arbitrary routing (M6). Node positions are GUI-only (persisted, no audio
    // effect). canAddConnection enforces: distinct nodes, stereo channels, no
    // edge into audioIn / out of audioOut, no duplicate, and no cycle.
    void  setNodePosition (const String& uid, float x, float y);
    float getNodeX (const String& uid) const;
    float getNodeY (const String& uid) const;
    bool  hasConnection    (const String& srcUid, int srcCh, const String& dstUid, int dstCh) const;
    void  removeConnection (const String& srcUid, int srcCh, const String& dstUid, int dstCh);
    bool  canAddConnection (const String& srcUid, int srcCh, const String& dstUid, int dstCh) const;

    // True iff the graph is a single unbranched series audioIn -> plugins -> audioOut
    // covering every plugin, with no extra/parallel edges. The tray serial-chain
    // ops (append/remove/move) are only safe — and only offered — when this holds.
    bool isLinearChain() const;

    //==============================================================================
    // Builds a document from the pre-2026 LightHost settings keys
    // (pluginListActive + plugin-order-*/plugin-bypass-*/plugin-state-*).
    // Returns an empty default document if there is nothing to migrate.
    static GraphDocument migrateFromLegacySettings (PropertiesFile&);

    // One-time v1 (normalised positions) -> v2 (px) migration of a GRAPH tree, shared by documents and
    // presets. Returns true if it changed anything; idempotent; a newer version is left untouched.
    static bool migrateTree (ValueTree& graph);

    bool migratedOnLoad = false;   // set by fromXml when the loaded tree was a v1 document

    ValueTree state;   // the GRAPH tree; public for future GUI/undo binding

private:
    bool pathExists (const String& fromUid, const String& toUid) const;   // node-level reachability
    String addNodeInternal (const String& type, const String& name, float x, float y);
};
