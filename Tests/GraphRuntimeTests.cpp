//
//  GraphRuntimeTests.cpp — console entry point for the headless runtime self-test.
//
//  The checks live in Source/Engine/SelfTest.{h,cpp} so the shipping app can run
//  the exact same verification via its `-run-selftest` switch. This target just
//  spins up a message manager and forwards any .vst3 paths given as arguments.
//
//  Build:  cmake -B build -DLIGHTHOST_BUILD_RUNTIME_TESTS=ON
//          cmake --build build --config Release --target LightHostRuntimeTests
//  Run:    "build/LightHostRuntimeTests_artefacts/Release/LightHostRuntimeTests.exe" [plugin.vst3 ...]
//

#include <JuceHeader.h>
#include "../Source/Engine/SelfTest.h"

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // message manager for plugin instantiation

    juce::StringArray pluginPaths;
    for (int i = 1; i < argc; ++i)
        pluginPaths.add (argv[i]);

    return lighthost::runSelfTest (pluginPaths);
}
