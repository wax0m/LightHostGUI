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
//      audioOut nodes may carry outCh="2" outLabel="Loop-back 1/2" outDevice="Audient EVO4" main="1"
//      (the pair of device output channels they feed; see OutputPairs.h).
//        <PREV_PLUGIN …/>                         (description before the first Replace; kept with
//                                                  prevState="base64…", both written once, never overwritten)
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
#include <set>

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
    String getIoNodeUid (bool input) const;           // output = the MAIN OUT (main="1", else the first audioOut)

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

    // Instruments (VST3 synths, "sources"): a plugin with no audio input. Judged from the stored
    // description, so a missing instrument still reads as one. A source has no input port, takes no
    // dry/wet mix and can't be the target of a connection.
    static bool isSourceDescription (const PluginDescription&);          // isInstrument || no input channels
    bool isSourceNode (const String& uid) const;
    // Sources whose out-edges all go to audioOut nodes (none at all counts too): they sit beside the
    // serial chain, so isLinearChain and the tray chain ops ignore them and their edges.
    std::set<String> getSideNodes() const;

    // Replace a plugin node's implementation in place (uid, wiring, strip, position, flags stay).
    // Sets PLUGIN + name + state; the first replace also keeps the original description and state
    // (PREV_PLUGIN + prevState) and later replaces never overwrite them. False for a non-plugin uid.
    bool replacePlugin (const String& uid, const PluginDescription& newDesc,
                        const String& newStateBase64, const String& originalStateBase64);
    String getPreviousPluginState (const String& uid) const;   // prevState, "" if never replaced

    // Key of a pre-2026 PropertiesFile entry: "plugin-<type>-<name><version><format>"
    // (type = order | bypass | state), e.g. plugin-state-TAL-Dub-3V1.1.0VST.
    static String legacyKey (const String& type, const PluginDescription&);

    //==============================================================================
    // Output nodes bound to device channel pairs. The document's original OUT is the main OUT and can't
    // be removed; extra OUTs are added/rebound/removed freely. A binding is the ABSOLUTE index of the
    // pair's left channel, a display label, and the device name it was made on (all three or none).
    String addOutputNode (int outCh, const String& label, const String& device, float x, float y);
    std::vector<String> getOutputUids() const;         // every audioOut, document order
    bool   isMainOutput (const String& uid) const;
    void   setOutputBinding (const String& uid, int outCh, const String& label, const String& device);
    int    getOutputChannel (const String& uid) const;  // -1 = not bound
    String getOutputLabel (const String& uid) const;
    String getOutputDevice (const String& uid) const;

    // Stamp every audioOut that has no binding with (outCh, label, device) and make sure one OUT is
    // main. Idempotent: a bound OUT (also one bound to another device) is never touched. True if anything
    // changed. The tree form serves stored presets.
    // Load-time repair: remove extra (never the main) OUT nodes with an impossible binding, cables included. A
    // binding is impossible when the pair doesn't exist on the device it was made on (`device` / `numChannels` are
    // the OPEN device; a binding made on another device can't be judged by them) or the channel is absurd on
    // any device. True if something was removed.
    bool removeInvalidOutputs (const String& device, int numChannels);
    static constexpr int maxOutputChannels = 1024;

    static bool pinLegacyOutputs (ValueTree& graph, int outCh, const String& label, const String& device);
    bool pinLegacyOutputs (int outCh, const String& label, const String& device) { return pinLegacyOutputs (state, outCh, label, device); }

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
