//
//  SelfTest.h — headless runtime self-test for the graph engine.
//
//  Exercises the three behaviours that need real plugin instantiation:
//    A. two instances of the SAME plugin coexist as distinct live nodes;
//    B. a bypass flag survives a save() -> fresh loadFrom() restart;
//    C. a pre-2026 legacy settings file migrates on first load, then persists.
//
//  Used by both the LightHostRuntimeTests console target and the shipping app's
//  `-run-selftest` command-line switch, so CI can verify the real binary.
//
//  The caller must have a JUCE message manager running (a ScopedJuceInitialiser_GUI
//  in a console tool, or the app's own message thread inside initialise()).
//

#pragma once

#include <JuceHeader.h>

namespace lighthost
{
    // explicitPluginPaths: optional .vst3 paths to test; empty => auto-discover
    // from the platform's standard VST3 folder. Prints a human-readable report.
    // Returns 0 on all-pass (or SKIP when no loadable plugin is found), 1 on any
    // failed check.
    int runSelfTest (const juce::StringArray& explicitPluginPaths);
}
