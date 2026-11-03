//
//  AutostartCommand.h — pure helpers for the "Start with Windows" Run-key value: composing the quoted command
//  and comparing it with an exe path. No registry access here (see Source/Autostart.h).
//

#pragma once

#include <JuceHeader.h>

namespace lighthost::autostart
{

inline const juce::String kRunValueName = "LightHost GUI";

enum class State { off, thisExe, otherExe };

// The Run value for an exe: its full path in double quotes.
inline juce::String makeRunValue (const juce::String& exePath)
{
    return "\"" + exePath + "\"";
}

// The exe path inside a Run value (surrounding whitespace and quotes removed, normalised for comparing).
inline juce::String normalisedExe (const juce::String& runValue)
{
    return runValue.trim().unquoted().trim().replaceCharacter ('/', '\\').toLowerCase();
}

inline State classify (const juce::String& runValue, const juce::String& exePath)
{
    if (runValue.trim().isEmpty())
        return State::off;
    return normalisedExe (runValue) == normalisedExe (exePath) ? State::thisExe : State::otherExe;
}

} // namespace lighthost::autostart
