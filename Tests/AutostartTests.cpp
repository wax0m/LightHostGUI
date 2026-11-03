//
//  AutostartTests.cpp — headless tests for the Run-value helpers (AutostartCommand.h). The registry is never
//  touched. Exposes runAutostartTests(int& checks).
//

#include "../Source/Engine/AutostartCommand.h"
#include <iostream>

using namespace juce;
namespace au = lighthost::autostart;

namespace
{
    int failures = 0;
    void check (int& checks, bool cond, const String& msg)
    {
        ++checks;
        if (! cond) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; }
    }
}

int runAutostartTests (int& checks)
{
    failures = 0;
    std::cout << "Autostart tests" << std::endl;

    const String exe = "C:\\Program Files\\Light Host\\LightHost GUI.exe";

    check (checks, au::kRunValueName == "LightHost GUI", "value name");
    check (checks, au::makeRunValue (exe) == "\"C:\\Program Files\\Light Host\\LightHost GUI.exe\"", "path with a space is quoted");
    check (checks, au::classify ({}, exe) == au::State::off, "no value: off");
    check (checks, au::classify ("  ", exe) == au::State::off, "blank value: off");
    check (checks, au::classify (au::makeRunValue (exe), exe) == au::State::thisExe, "own quoted value: this exe");
    check (checks, au::classify (exe, exe) == au::State::thisExe, "unquoted value of the same path: this exe");
    check (checks, au::classify ("\"c:\\program files\\light host\\lighthost gui.exe\"", exe) == au::State::thisExe, "case-insensitive");
    check (checks, au::classify ("\"C:/Program Files/Light Host/LightHost GUI.exe\"", exe) == au::State::thisExe, "forward slashes tolerated");
    check (checks, au::classify (" \"" + exe + "\" ", exe) == au::State::thisExe, "surrounding whitespace tolerated");
    check (checks, au::classify ("\"D:\\Other\\LightHost GUI.exe\"", exe) == au::State::otherExe, "another path: other copy");
    check (checks, au::classify ("\"" + exe + "\" -safe-mode", exe) == au::State::otherExe, "extra arguments: not this exact command");

    return failures;
}
