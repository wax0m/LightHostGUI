//
//  PresetTests.cpp — headless tests for the M8 preset store (named graph
//  documents + active index, deep-copied, versioned XML round-trip).
//

#include "../Source/Engine/PresetStore.h"
#include "../Source/Engine/SettingsBackup.h"
#include <iostream>

using namespace juce;

namespace
{
    int failures = 0;
    void check (int& checks, bool cond, const String& msg)
    {
        ++checks;
        if (! cond) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; }
    }
    GraphDocument docWith (int plugins)
    {
        GraphDocument d;
        for (int i = 0; i < plugins; ++i)
        {
            PluginDescription pd; pd.name = "P" + String (i); pd.pluginFormatName = "VST3"; pd.numInputChannels = pd.numOutputChannels = 2;
            d.addPluginNode (pd, 0.5f, 0.5f);
        }
        return d;                          // 2 IO + `plugins` nodes
    }
}

int runPresetTests (int& checks)
{
    failures = 0;
    std::cout << "Preset store tests" << std::endl;

    PresetStore store;
    check (checks, store.isValid(),            "fresh store is valid");
    check (checks, store.getNumPresets() == 0, "fresh store has no presets");
    check (checks, store.getActiveIndex() == -1, "fresh store has no active preset");

    const int ia = store.addPreset ("Vocal Chain", docWith (1));
    const int ib = store.addPreset ("Drum Bus",    docWith (2));
    check (checks, ia == 0 && ib == 1,          "addPreset returns sequential indices");
    check (checks, store.getNumPresets() == 2,  "two presets stored");
    check (checks, store.getActiveIndex() == 0, "first added preset becomes active");
    check (checks, store.getPresetName (0) == "Vocal Chain" && store.getPresetName (1) == "Drum Bus",
           "preset names preserved");
    check (checks, store.getPresetDocument (1).getNodes().getNumChildren() == 4,
           "preset B document has 2 IO + 2 plugin nodes");

    // Deep-copy independence: editing a retrieved doc must not touch the store.
    {
        GraphDocument got = store.getPresetDocument (0);
        PluginDescription extra; extra.name = "X"; extra.pluginFormatName = "VST3"; extra.numInputChannels = extra.numOutputChannels = 2;
        got.addPluginNode (extra, 0.5f, 0.5f);
        check (checks, store.getPresetDocument (0).getNodes().getNumChildren() == 3,
               "store is unaffected by edits to a retrieved document (deep copy)");
    }

    store.setActiveIndex (1);
    check (checks, store.getActiveIndex() == 1, "active index settable");
    store.setPresetName (0, "Lead Vox");
    check (checks, store.getPresetName (0) == "Lead Vox", "preset rename works");

    store.setPresetDocument (0, docWith (3));
    check (checks, store.getPresetDocument (0).getNodes().getNumChildren() == 5,
           "setPresetDocument overwrites the stored graph");

    // Versioned XML round-trip.
    PresetStore rt = PresetStore::fromXml (store.toXml());
    check (checks, rt.isValid(),                 "round-tripped store is valid");
    check (checks, rt.getNumPresets() == 2,      "preset count survives round-trip");
    check (checks, rt.getActiveIndex() == 1,     "active index survives round-trip");
    check (checks, rt.getPresetName (0) == "Lead Vox", "names survive round-trip");
    check (checks, rt.getPresetDocument (0).getNodes().getNumChildren() == 5,
           "stored documents survive round-trip");
    check (checks, PresetStore::fromXml ("<NOPE/>").isValid() == false, "garbage XML => invalid store");

    store.removePreset (0);
    check (checks, store.getNumPresets() == 1 && store.getPresetName (0) == "Drum Bus",
           "removePreset drops the right one");

    // --- Unreadable settings are backed up, never lost ------------------------
    {
        const auto dir = File::getSpecialLocation (File::tempDirectory).getChildFile ("lighthost-backup-test");
        dir.deleteRecursively();
        dir.createDirectory();
        PropertiesFile::Options o;
        o.commonToAllUsers = false;
        PropertiesFile props (dir.getChildFile ("test.settings"), o);

        const String raw = "<GRAPH broken";
        const auto backup = lighthost::backupUnreadable (props, "graphDocument", raw);
        check (checks, backup.existsAsFile(), "backup: file written");
        check (checks, backup.loadFileAsString() == raw, "backup: file holds the exact raw text");
        check (checks, backup.getFileName().contains ("graphDocument-unreadable-"), "backup: file name carries the key");
        check (checks, props.getValue ("graphDocument.unreadable") == raw, "backup: settings key holds the raw text");
        check (checks, ! GraphDocument::fromXml (raw).isValid(), "backup: the sample really is unreadable");
        dir.deleteRecursively();
    }

    std::cout << (failures == 0 ? "  presets ok" : "  presets FAILED") << std::endl;
    // v1 presets (normalised positions) are migrated to px when the store is loaded.
    {
        const auto v1Graph = [] (float px)
        {
            return String ("<GRAPH version=\"1\"><NODES>")
                 + "<NODE uid=\"I\" type=\"audioIn\" name=\"i\" x=\"0.05\" y=\"0.5\"/>"
                 + "<NODE uid=\"P\" type=\"plugin\" name=\"p\" x=\"" + String (px) + "\" y=\"0.5\"/>"
                 + "<NODE uid=\"O\" type=\"audioOut\" name=\"o\" x=\"0.95\" y=\"0.5\"/>"
                 + "</NODES><CONNECTIONS/></GRAPH>";
        };
        const String xml = "<PRESETS version=\"1\" active=\"0\">"
                           "<PRESET name=\"A\">" + v1Graph (0.35f) + "</PRESET>"
                           "<PRESET name=\"B\">" + v1Graph (0.65f) + "</PRESET></PRESETS>";
        const auto migrated = PresetStore::fromXml (xml);
        check (checks, migrated.isValid() && migrated.migratedOnLoad, "v1 presets: store is valid and flagged as migrated");
        const auto a = migrated.getPresetDocument (0);
        const auto b = migrated.getPresetDocument (1);
        check (checks, (int) a.state.getProperty ("version") == 2 && (int) b.state.getProperty ("version") == 2,
               "v1 presets: both graphs are version 2");
        check (checks, std::abs (a.getNodeX ("P") - 462.0f) < 0.6f && std::abs (b.getNodeX ("P") - 738.0f) < 0.6f,
               "v1 presets: plugin positions are px");
        check (checks, ! PresetStore::fromXml (migrated.toXml()).migratedOnLoad, "migrated presets are not migrated again");
    }

    // Remove: the active pointer follows its preset when an earlier one goes away.
    {
        PresetStore s;
        for (const char* n : { "A", "B", "C", "D" })
            s.addPreset (n, docWith (1));
        s.setActiveIndex (2);
        s.removePreset (0);
        check (checks, s.getActiveIndex() == 1 && s.getPresetName (1) == "C", "removing a preset before the active one keeps it active");
        s.removePreset (2);
        check (checks, s.getActiveIndex() == 1 && s.getPresetName (1) == "C", "removing a preset after the active one keeps it active");
        s.removePreset (1);
        check (checks, s.getNumPresets() == 1 && s.getActiveIndex() == 0 && s.getPresetName (0) == "B",
               "removing the active last preset clamps to the new last one");
        s.duplicatePreset (0);
        const int dup = s.duplicatePreset (1);   // a tab that is not active
        check (checks, dup == 2 && s.getActiveIndex() == 0 && s.getPresetName (0) == "B",
               "duplicating an inactive tab leaves the active preset alone");
    }

    // Duplicate: deep copy right behind the source, unique "copy" names, the active preset stays itself.
    {
        PresetStore s;
        s.addPreset ("A", docWith (1));
        s.addPreset ("B", docWith (2));
        s.addPreset ("C", docWith (3));
        s.setActiveIndex (2);

        check (checks, s.uniqueCopyName ("A") == "A copy", "copy name: first copy is '<name> copy'");
        const int c1 = s.duplicatePreset (0);
        check (checks, c1 == 1 && s.getNumPresets() == 4, "duplicate lands right behind its source");
        check (checks, s.getPresetName (1) == "A copy" && s.getPresetName (2) == "B", "duplicate is named '<name> copy'");
        check (checks, s.getActiveIndex() == 3 && s.getPresetName (3) == "C",
               "duplicating before the active preset keeps the active preset itself");

        const int c2 = s.duplicatePreset (0);
        check (checks, c2 == 1 && s.getPresetName (1) == "A copy 2" && s.getPresetName (2) == "A copy",
               "second copy is '<name> copy 2'");
        s.setPresetName (3, "a COPY 3");
        check (checks, s.uniqueCopyName ("A") == "A copy 4", "copy names skip taken ones, case-insensitively");

        // Deep copy: editing one side leaves the other alone.
        s.setPresetDocument (1, docWith (5));
        check (checks, s.getPresetDocument (1).getNodes().getNumChildren() == 7
                    && s.getPresetDocument (0).getNodes().getNumChildren() == 3
                    && s.getPresetDocument (2).getNodes().getNumChildren() == 3,
               "duplicate does not share data with its source");

        check (checks, s.duplicatePreset (-1) == -1 && s.duplicatePreset (99) == -1, "duplicate of a bad index is refused");

        const PresetStore round = PresetStore::fromXml (s.toXml());
        check (checks, round.getNumPresets() == s.getNumPresets() && round.getActiveIndex() == s.getActiveIndex()
                    && round.getPresetName (1) == "A copy 2" && round.getPresetName (2) == "A copy",
               "copies survive the XML round-trip in order");
    }

    // New Preset: an empty IN -> OUT document; Duplicate still copies.
    {
        PresetStore s;
        s.addPreset ("A", docWith (2));
        const auto empty = PresetStore::emptyPresetDocument();
        check (checks, empty.isValid(), "empty preset: document is valid");
        check (checks, empty.getNodes().getNumChildren() == 2, "empty preset: only IN and OUT");
        check (checks, empty.getConnections().getNumChildren() == 2, "empty preset: two connections (a stereo pair)");
        const auto in = empty.getIoNodeUid (true), out = empty.getIoNodeUid (false);
        check (checks, empty.hasConnection (in, 0, out, 0) && empty.hasConnection (in, 1, out, 1),
               "empty preset: IN feeds the main OUT on both channels");
        bool anyPlugin = false;
        for (auto n : empty.getNodes())
            anyPlugin = anyPlugin || n.getProperty ("type").toString() == "plugin";
        check (checks, ! anyPlugin, "empty preset: no plugin node");

        const int idx = s.addPreset ("New", empty);
        check (checks, s.getPresetDocument (idx).getNodes().getNumChildren() == 2
                    && s.getPresetDocument (0).getNodes().getNumChildren() == 4,
               "adding the empty preset leaves the others alone");
        const int dup = s.duplicatePreset (0);
        check (checks, s.getPresetDocument (dup).getNodes().getNumChildren() == 4, "duplicate still copies the plugins");
    }

    // movePreset: the active preset stays itself; bad indices change nothing.
    {
        const auto names = [] (const PresetStore& st)
        {
            String r;
            for (int i = 0; i < st.getNumPresets(); ++i)
                r << st.getPresetName (i);
            return r;
        };
        const auto fresh = [&] (int active)
        {
            PresetStore st;
            for (const char* n : { "A", "B", "C", "D" })
                st.addPreset (n, docWith (1));
            st.setActiveIndex (active);
            return st;
        };

        auto s = fresh (2);   // C active
        check (checks, s.movePreset (0, 3) && names (s) == "BCDA" && s.getActiveIndex() == 1,
               "move: an earlier tab past the active one shifts the active index down");
        s = fresh (2);
        check (checks, s.movePreset (3, 0) && names (s) == "DABC" && s.getActiveIndex() == 3,
               "move: a later tab before the active one shifts the active index up");
        s = fresh (2);
        check (checks, s.movePreset (2, 0) && names (s) == "CABD" && s.getActiveIndex() == 0,
               "move: the active tab itself carries the active index");
        s = fresh (0);
        check (checks, s.movePreset (1, 2) && names (s) == "ACBD" && s.getActiveIndex() == 0,
               "move: a move that does not cross the active tab leaves its index");
        s = fresh (1);
        check (checks, s.movePreset (0, 1) && names (s) == "BACD" && s.getActiveIndex() == 0,
               "move: swapping with the active neighbour");

        s = fresh (1);
        check (checks, ! s.movePreset (1, 1) && ! s.movePreset (-1, 2) && ! s.movePreset (4, 0)
                    && ! s.movePreset (0, -1) && ! s.movePreset (0, 4),
               "move: same or invalid indices are refused");
        check (checks, names (s) == "ABCD" && s.getActiveIndex() == 1, "move: a refused move changes nothing");

        s = fresh (3);
        s.movePreset (0, 3);
        const PresetStore round = PresetStore::fromXml (s.toXml());
        check (checks, names (round) == "BCDA" && round.getActiveIndex() == 2
                    && round.getPresetDocument (3).getNodes().getNumChildren() == 3,
               "move: order, active preset and documents survive the XML round-trip");
    }

    // Shortcuts: stored on the preset, one combination for at most one preset.
    {
        namespace hk = lighthost::hotkeys;
        PresetStore s;
        for (const char* n : { "A", "B", "C" })
            s.addPreset (n, docWith (1));
        s.setActiveIndex (1);

        check (checks, s.getPresetShortcut (0).isEmpty() && s.registeredShortcuts().isEmpty(), "shortcut: none by default");
        check (checks, s.setPresetShortcut (0, *hk::parse ("alt+ctrl+1")) == -1 && s.getPresetShortcut (0) == "Ctrl+Alt+1",
               "shortcut: bound and stored in canonical form");
        check (checks, s.findPresetByShortcut (*hk::parse ("Ctrl+Alt+1")) == 0 && s.findPresetByShortcut (*hk::parse ("Ctrl+Alt+2")) == -1,
               "shortcut: found by combination");

        // Taking over a combination moves it and reports the previous owner.
        check (checks, s.setPresetShortcut (2, *hk::parse ("Ctrl+Alt+1")) == 0, "shortcut: the previous owner is reported");
        check (checks, s.getPresetShortcut (0).isEmpty() && s.getPresetShortcut (2) == "Ctrl+Alt+1",
               "shortcut: a taken combination moves to the new preset");
        check (checks, s.setPresetShortcut (2, *hk::parse ("Ctrl+Alt+1")) == -1, "shortcut: rebinding the same combination to its owner is no conflict");

        // Invalid bindings change nothing.
        hk::Combo bare; bare.key = "A";
        check (checks, s.setPresetShortcut (1, bare) == -1 && s.getPresetShortcut (1).isEmpty(), "shortcut: a key without modifier is refused");
        check (checks, s.setPresetShortcut (9, *hk::parse ("Ctrl+Alt+7")) == -1 && s.registeredShortcuts().size() == 1,
               "shortcut: a bad preset index is refused");

        // It follows the preset through rename and move, and survives the XML round-trip.
        s.setPresetShortcut (0, *hk::parse ("Shift+F2"));
        s.setPresetName (0, "Renamed");
        s.movePreset (0, 2);   // B, C, Renamed
        check (checks, s.getPresetName (2) == "Renamed" && s.getPresetShortcut (2) == "Shift+F2" && s.getPresetShortcut (1) == "Ctrl+Alt+1",
               "shortcut: follows the preset through rename and move");
        const PresetStore round = PresetStore::fromXml (s.toXml());
        check (checks, round.getPresetShortcut (1) == "Ctrl+Alt+1" && round.getPresetShortcut (2) == "Shift+F2"
                    && round.registeredShortcuts().size() == 2, "shortcut: survives the XML round-trip");

        // Duplicate does not copy it; delete frees it; clear removes it.
        const int dup = s.duplicatePreset (1);
        check (checks, s.getPresetShortcut (dup).isEmpty() && s.getPresetShortcut (1) == "Ctrl+Alt+1", "shortcut: a duplicate starts without one");
        s.removePreset (1);
        check (checks, s.findPresetByShortcut (*hk::parse ("Ctrl+Alt+1")) == -1, "shortcut: deleting the preset frees its combination");
        const int idx = s.findPresetByShortcut (*hk::parse ("Shift+F2"));
        s.clearPresetShortcut (idx);
        check (checks, s.getPresetShortcut (idx).isEmpty() && s.registeredShortcuts().isEmpty(), "shortcut: cleared");

        // A hand-edited, unreadable value counts as none.
        PresetStore bad = PresetStore::fromXml ("<PRESETS version=\"1\" active=\"0\"><PRESET name=\"X\" shortcut=\"Banana\"/>"
                                               "<PRESET name=\"Y\" shortcut=\"ctrl+9\"/><PRESET name=\"Z\" shortcut=\"Ctrl+9\"/></PRESETS>");
        check (checks, bad.getPresetShortcut (0).isEmpty() && bad.getPresetShortcut (1) == "Ctrl+9", "shortcut: an unreadable stored value is ignored");
        check (checks, bad.registeredShortcuts().size() == 1 && bad.findPresetByShortcut (*hk::parse ("Ctrl+9")) == 1,
               "shortcut: a duplicate stored combination is registered once");
    }

    return failures;
}
