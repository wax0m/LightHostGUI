//
//  ModulePinTests.cpp — headless tests for the pure parts of ModulePin.h: which binary a plugin path stands for
//  and the once-per-path bookkeeping. The real pin (a DLL that survives FreeLibrary) is Test Y in SelfTest.cpp.
//  Exposes runModulePinTests(int& checks): appends to the check count, returns failures.
//

#include "../Source/Engine/ModulePin.h"
#include <iostream>

using namespace juce;
namespace mp = lighthost::modules;

namespace
{
    int failures = 0;
    void check (int& checks, bool cond, const String& msg)
    {
        ++checks;
        if (! cond) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; }
    }
}

int runModulePinTests (int& checks)
{
    failures = 0;
    std::cout << "Module pin tests" << std::endl;

    const auto dir = File::getSpecialLocation (File::tempDirectory)
                         .getChildFile ("lighthost-pin-test-" + String::toHexString (Random::getSystemRandom().nextInt64()));
    dir.createDirectory();

    // A single-file .vst3 is its own binary.
    {
        const auto file = dir.getChildFile ("Single.vst3");
        file.replaceWithText ("x");
        check (checks, mp::binaryOfPlugin (file) == file, "binary: a single-file plugin is its own binary");
    }

    // A bundle folder holds Contents\x86_64-win\<same name>.
    {
        const auto bundle = dir.getChildFile ("Bundle.vst3");
        bundle.getChildFile ("Contents").getChildFile ("x86_64-win").createDirectory();
        const auto expected = bundle.getChildFile ("Contents").getChildFile ("x86_64-win").getChildFile ("Bundle.vst3");
        check (checks, mp::binaryOfPlugin (bundle) == expected, "binary: a bundle folder resolves to Contents\\x86_64-win\\<name>");
    }

    // A path that does not exist is taken as a file.
    {
        const auto ghost = dir.getChildFile ("Ghost.vst3");
        check (checks, mp::binaryOfPlugin (ghost) == ghost, "binary: a missing path is taken as a file");
    }

    // Once per path, case-insensitively; forgetting makes a retry possible.
    {
        mp::PinnedSet set;
        const File a ("C:\\VST3\\Alpha.vst3");
        check (checks, ! set.contains (a), "set: starts empty");
        check (checks, set.insert (a), "set: the first insert is new");
        check (checks, ! set.insert (a), "set: the second insert is not");
        check (checks, ! set.insert (File ("C:\\VST3\\ALPHA.vst3")), "set: paths are compared without regard to case");
        check (checks, set.contains (a), "set: contains the path");
        set.forget (a);
        check (checks, ! set.contains (a) && set.insert (a), "set: after forget the path can be inserted again");
    }

   #if JUCE_WINDOWS
    // The process-wide pin refuses a path that is not loaded, and does not remember it.
    {
        const auto ghost = dir.getChildFile ("Never Loaded.vst3");
        check (checks, ! mp::pinPluginModule (ghost), "pin: a module that is not loaded pins nothing");
        check (checks, ! mp::pinnedSet().contains (ghost), "pin: ... and is not remembered, so a later load can still be pinned");
    }
   #endif

    dir.deleteRecursively();
    return failures;
}
