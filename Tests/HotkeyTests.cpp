//
//  HotkeyTests.cpp — headless tests for the pure shortcut helpers (HotkeyCombo.h). RegisterHotKey is never called.
//  Exposes runHotkeyTests(int& checks).
//

#include "../Source/Engine/HotkeyCombo.h"
#include <iostream>

using namespace juce;
namespace hk = lighthost::hotkeys;

namespace
{
    int failures = 0;
    void check (int& checks, bool cond, const String& msg)
    {
        ++checks;
        if (! cond) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; }
    }
}

int runHotkeyTests (int& checks)
{
    failures = 0;
    std::cout << "Hotkey tests" << std::endl;

    // parse / format
    {
        const auto c = hk::parse ("ctrl+alt+f5");
        check (checks, c.has_value() && c->ctrl && c->alt && ! c->shift && ! c->win && c->key == "F5", "parse: modifiers and F-key");
        check (checks, c.has_value() && hk::format (*c) == "Ctrl+Alt+F5", "format: canonical spelling");

        const auto d = hk::parse (" Win + shift + a ");
        check (checks, d.has_value() && hk::format (*d) == "Shift+Win+A", "parse: spaces, case and order are normalised");
        check (checks, hk::parse (hk::format (*d)) == d, "format -> parse round-trip");

        check (checks, hk::parse ("Control+1").has_value() && hk::format (*hk::parse ("Control+1")) == "Ctrl+1", "parse: Control is Ctrl");
        check (checks, hk::parse ("Alt+num7").has_value() && hk::format (*hk::parse ("Alt+num7")) == "Alt+Num7", "parse: numpad token");
        check (checks, hk::parse ("Ctrl+pageup").has_value() && hk::format (*hk::parse ("Ctrl+pageup")) == "Ctrl+PageUp", "parse: named key");
        check (checks, hk::parse ("Ctrl+F24").has_value() && ! hk::parse ("Ctrl+F25").has_value() && ! hk::parse ("Ctrl+F0").has_value()
                    && ! hk::parse ("Ctrl+F01").has_value(), "parse: F1..F24 only");
    }

    // rejected input
    {
        check (checks, ! hk::parse ("A").has_value(),            "parse: a key without a modifier is refused");
        check (checks, ! hk::parse ("Ctrl+Alt").has_value(),     "parse: modifiers only are refused");
        check (checks, ! hk::parse ("Ctrl+A+B").has_value(),     "parse: two keys are refused");
        check (checks, ! hk::parse ("Ctrl+Banana").has_value(),  "parse: an unknown key is refused");
        check (checks, ! hk::parse ("").has_value() && ! hk::parse ("+").has_value() && ! hk::parse ("Ctrl+").has_value(),
               "parse: empty and dangling input is refused");
    }

    // key tokens from JUCE key codes
    {
        using juce::KeyPress;
        check (checks, hk::keyTokenFor ('a') == "A" && hk::keyTokenFor ('A') == "A", "token: letters, either case");
        check (checks, hk::keyTokenFor ('7') == "7", "token: digit");
        check (checks, hk::keyTokenFor (KeyPress::F1Key) == "F1" && hk::keyTokenFor (KeyPress::F12Key) == "F12"
                    && hk::keyTokenFor (KeyPress::F24Key) == "F24", "token: function keys");
        check (checks, hk::keyTokenFor (KeyPress::numberPad0) == "Num0" && hk::keyTokenFor (KeyPress::numberPad9) == "Num9", "token: numpad");
        check (checks, hk::keyTokenFor (KeyPress::spaceKey) == "Space" && hk::keyTokenFor (KeyPress::insertKey) == "Insert"
                    && hk::keyTokenFor (KeyPress::deleteKey) == "Delete" && hk::keyTokenFor (KeyPress::homeKey) == "Home"
                    && hk::keyTokenFor (KeyPress::endKey) == "End" && hk::keyTokenFor (KeyPress::pageUpKey) == "PageUp"
                    && hk::keyTokenFor (KeyPress::pageDownKey) == "PageDown", "token: navigation keys");
        check (checks, hk::keyTokenFor (KeyPress::upKey) == "Up" && hk::keyTokenFor (KeyPress::downKey) == "Down"
                    && hk::keyTokenFor (KeyPress::leftKey) == "Left" && hk::keyTokenFor (KeyPress::rightKey) == "Right", "token: arrows");
        check (checks, hk::keyTokenFor (KeyPress::escapeKey).isEmpty() && hk::keyTokenFor (KeyPress::returnKey).isEmpty()
                    && hk::keyTokenFor (0).isEmpty() && hk::keyTokenFor (-1).isEmpty(), "token: unsupported keys give nothing");

        const auto kp = hk::fromKeyPress (KeyPress ('k', ModifierKeys (ModifierKeys::ctrlModifier | ModifierKeys::shiftModifier), 'K'), true);
        check (checks, kp.has_value() && hk::format (*kp) == "Ctrl+Shift+Win+K", "fromKeyPress: modifiers, Win flag and key");
        const auto plain = hk::fromKeyPress (KeyPress ('k'), false);
        check (checks, plain.has_value() && ! plain->hasModifier(), "fromKeyPress: no modifier is reported as such");
        check (checks, ! hk::fromKeyPress (KeyPress (KeyPress::escapeKey), false).has_value(), "fromKeyPress: unsupported key gives nothing");
    }

    // virtual keys give layout-independent tokens (a Czech digit row types '+', ě, š... but its VKs are 1..0)
    {
        check (checks, hk::tokenForVirtualKey (0x31) == "1" && hk::tokenForVirtualKey (0x41) == "A" && hk::tokenForVirtualKey (0x74) == "F5"
                    && hk::tokenForVirtualKey (0x63) == "Num3" && hk::tokenForVirtualKey (0x25) == "Left" && hk::tokenForVirtualKey (0x20) == "Space",
               "vk token: digits, letters, F-keys, numpad, navigation");
        check (checks, hk::tokenForVirtualKey (0x10).isEmpty() && hk::tokenForVirtualKey (0x0D).isEmpty() && hk::tokenForVirtualKey (0x1B).isEmpty()
                    && hk::tokenForVirtualKey (0).isEmpty() && hk::tokenForVirtualKey (0x88).isEmpty(), "vk token: modifiers and unsupported keys give nothing");
        bool roundTrip = true;
        for (int vk = 0x20; vk <= 0x87; ++vk)
            if (const auto t = hk::tokenForVirtualKey (vk); t.isNotEmpty())
                roundTrip = roundTrip && hk::toWin32 (*hk::parse ("Ctrl+" + t)).vk == (unsigned) vk;
        check (checks, roundTrip, "vk token: every token maps back to its virtual key");

        const auto cz = hk::fromKeyPress (juce::KeyPress ('+', juce::ModifierKeys (juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier), '+'),
                                          false, hk::tokenForVirtualKey (0x31));
        check (checks, cz.has_value() && hk::format (*cz) == "Ctrl+Alt+1", "fromKeyPress: the virtual-key token wins over the typed character");
    }

    // Win32 arguments
    {
        const auto a = hk::toWin32 (*hk::parse ("Ctrl+Alt+F5"));
        check (checks, a.vk == 0x74 && a.mods == (0x0001u | 0x0002u | 0x4000u), "win32: Ctrl+Alt+F5 = MOD_ALT|MOD_CONTROL|MOD_NOREPEAT, VK_F5");
        const auto b = hk::toWin32 (*hk::parse ("Win+Shift+A"));
        check (checks, b.vk == 0x41 && b.mods == (0x0008u | 0x0004u | 0x4000u), "win32: Win+Shift+A");
        const auto n = hk::toWin32 (*hk::parse ("Ctrl+Num3"));
        check (checks, n.vk == 0x63, "win32: numpad 3 = VK_NUMPAD3");
        const auto d = hk::toWin32 (*hk::parse ("Alt+5"));
        check (checks, d.vk == 0x35, "win32: digit keys use their ASCII code");
        check (checks, hk::toWin32 (*hk::parse ("Ctrl+Left")).vk == 0x25 && hk::toWin32 (*hk::parse ("Ctrl+Down")).vk == 0x28
                    && hk::toWin32 (*hk::parse ("Ctrl+Delete")).vk == 0x2E && hk::toWin32 (*hk::parse ("Ctrl+PageDown")).vk == 0x22
                    && hk::toWin32 (*hk::parse ("Ctrl+Space")).vk == 0x20 && hk::toWin32 (*hk::parse ("Ctrl+F24")).vk == 0x87,
               "win32: navigation keys and F24");

        hk::Combo noMod; noMod.key = "A";
        hk::Combo noKey; noKey.ctrl = true;
        check (checks, hk::toWin32 (noMod).vk == 0 && hk::toWin32 (noKey).vk == 0, "win32: an invalid combination is not registrable");
    }

    // conflict lookup
    {
        const StringArray bindings { "", "Ctrl+Alt+1", "alt+ctrl+2" };
        check (checks, hk::findOwner (bindings, *hk::parse ("Ctrl+Alt+1")) == 1, "owner: exact binding");
        check (checks, hk::findOwner (bindings, *hk::parse ("Ctrl+Alt+2")) == 2, "owner: spelling and order do not matter");
        check (checks, hk::findOwner (bindings, *hk::parse ("Ctrl+Alt+3")) == -1, "owner: a free combination has none");
        check (checks, hk::findOwner ({}, *hk::parse ("Ctrl+Alt+3")) == -1, "owner: no bindings at all");
    }

    std::cout << (failures == 0 ? "  hotkeys ok" : "  hotkeys FAILED") << std::endl;
    return failures;
}
