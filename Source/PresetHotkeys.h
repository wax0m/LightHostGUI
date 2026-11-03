//
//  PresetHotkeys.h — system-wide hotkeys (Windows RegisterHotKey on a hidden message-only window). The window and the
//  registrations exist only while at least one combination is registered; with none, nothing is created. A press is
//  never handled inside the window procedure: it is posted to the message queue (MessageManager::callAsync) and
//  delivered to onPressed from there. Message thread only. On other platforms every call is a no-op.
//

#pragma once

#include <JuceHeader.h>

class PresetHotkeys
{
public:
    PresetHotkeys();
    ~PresetHotkeys();

    // Called on the message thread, outside window-message handling, with the canonical text of the combination.
    std::function<void (const juce::String&)> onPressed;

    // Replaces ALL registrations with `combos` (canonical texts) and returns those that could not be registered
    // (another app holds them, or they are unreadable). An empty list unregisters everything and destroys the window.
    juce::StringArray sync (const juce::StringArray& combos);

    // The supported key (canonical token, see HotkeyCombo.h) that is down right now, read from virtual-key codes so it
    // does not depend on the keyboard layout; "" when none (or off Windows).
    static juce::String pressedKeyToken();
    static bool isWinKeyDown();   // either Windows key is down right now (a JUCE key press does not carry it)

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetHotkeys)
};
