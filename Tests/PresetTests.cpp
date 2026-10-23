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
            PluginDescription pd; pd.name = "P" + String (i); pd.pluginFormatName = "VST3";
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
        PluginDescription extra; extra.name = "X"; extra.pluginFormatName = "VST3";
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

    return failures;
}
