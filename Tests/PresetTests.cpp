//
//  PresetTests.cpp — headless tests for the M8 preset store (named graph
//  documents + active index, deep-copied, versioned XML round-trip).
//

#include "../Source/Engine/PresetStore.h"
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

    std::cout << (failures == 0 ? "  presets ok" : "  presets FAILED") << std::endl;
    return failures;
}
