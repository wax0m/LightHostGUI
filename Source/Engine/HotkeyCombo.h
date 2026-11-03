//
//  HotkeyCombo.h — pure helpers for the global preset shortcuts: a key combination (modifiers + one key) parsed from
//  and formatted to its canonical text ("Ctrl+Alt+F5"), mapped from a JUCE key press and to the Win32 RegisterHotKey
//  arguments, and the "who already owns this combination" lookup. No windows.h and no registration here
//  (see Source/PresetHotkeys.h), so everything is headless-testable.
//

#pragma once

#include <JuceHeader.h>
#include <optional>

namespace lighthost::hotkeys
{

struct Combo
{
    bool ctrl = false, alt = false, shift = false, win = false;
    juce::String key;   // canonical token: "A".."Z", "0".."9", "F1".."F24", "Num0".."Num9", "Space", "Insert", ...

    bool hasModifier() const { return ctrl || alt || shift || win; }

    bool operator== (const Combo& o) const
    {
        return ctrl == o.ctrl && alt == o.alt && shift == o.shift && win == o.win && key == o.key;
    }
};

// Win32 values written out literally, so this header (and its tests) never needs windows.h.
inline constexpr unsigned modAlt = 0x0001, modControl = 0x0002, modShift = 0x0004, modWin = 0x0008, modNoRepeat = 0x4000;

struct Win32Args { unsigned mods = 0; unsigned vk = 0; };   // vk == 0: not a registrable combination

namespace detail
{
    struct NamedKey { const char* token; int vk; };

    inline const NamedKey* namedKeys (int& count)
    {
        static const NamedKey keys[] = {
            { "Space", 0x20 }, { "PageUp", 0x21 }, { "PageDown", 0x22 }, { "End", 0x23 }, { "Home", 0x24 },
            { "Left", 0x25 }, { "Up", 0x26 }, { "Right", 0x27 }, { "Down", 0x28 }, { "Insert", 0x2D }, { "Delete", 0x2E },
        };
        count = (int) (sizeof (keys) / sizeof (keys[0]));
        return keys;
    }

    // A token (any case) -> its canonical spelling; empty when it is not a supported key.
    inline juce::String canonicalKey (const juce::String& raw)
    {
        const auto t = raw.trim();
        if (t.length() == 1)
        {
            const auto c = juce::CharacterFunctions::toUpperCase (t[0]);
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                return juce::String::charToString (c);
        }
        if (t.length() >= 2 && juce::CharacterFunctions::toUpperCase (t[0]) == 'F' && t.substring (1).containsOnly ("0123456789"))
        {
            const int n = t.substring (1).getIntValue();
            if (n >= 1 && n <= 24 && t.substring (1) == juce::String (n))
                return "F" + juce::String (n);
        }
        if (t.length() == 4 && t.substring (0, 3).equalsIgnoreCase ("Num") && t[3] >= '0' && t[3] <= '9')
            return "Num" + t.substring (3);

        int count = 0;
        for (const auto* k = namedKeys (count); k != namedKeys (count) + count; ++k)
            if (t.equalsIgnoreCase (k->token))
                return k->token;
        return {};
    }

    inline int virtualKeyFor (const juce::String& canonical)
    {
        if (canonical.length() == 1)
            return (int) canonical[0];   // 'A'..'Z' and '0'..'9' are their own virtual-key codes
        if (canonical.startsWith ("Num"))
            return 0x60 + canonical.substring (3).getIntValue();
        if (canonical[0] == 'F' && canonical.length() >= 2 && canonical.substring (1).containsOnly ("0123456789"))
            return 0x70 + canonical.substring (1).getIntValue() - 1;
        int count = 0;
        for (const auto* k = namedKeys (count); k != namedKeys (count) + count; ++k)
            if (canonical == k->token)
                return k->vk;
        return 0;
    }
}

// "ctrl+alt+f5", "Alt + Shift + A", ... -> a combination; nullopt unless there is at least one modifier, exactly one
// supported key and nothing else.
inline std::optional<Combo> parse (const juce::String& text)
{
    Combo c;
    bool haveKey = false;
    for (const auto& part : juce::StringArray::fromTokens (text, "+", {}))
    {
        const auto p = part.trim();
        if (p.equalsIgnoreCase ("ctrl") || p.equalsIgnoreCase ("control"))  { c.ctrl = true; continue; }
        if (p.equalsIgnoreCase ("alt"))                                     { c.alt = true; continue; }
        if (p.equalsIgnoreCase ("shift"))                                   { c.shift = true; continue; }
        if (p.equalsIgnoreCase ("win") || p.equalsIgnoreCase ("windows"))   { c.win = true; continue; }

        const auto k = detail::canonicalKey (p);
        if (k.isEmpty() || haveKey)
            return std::nullopt;
        c.key = k;
        haveKey = true;
    }
    if (! haveKey || ! c.hasModifier())
        return std::nullopt;
    return c;
}

inline juce::String format (const Combo& c)
{
    juce::String s;
    if (c.ctrl)  s << "Ctrl+";
    if (c.alt)   s << "Alt+";
    if (c.shift) s << "Shift+";
    if (c.win)   s << "Win+";
    return s + c.key;
}

// Win32 virtual-key code -> canonical token (the inverse of virtualKeyFor); empty when unsupported. Layout independent:
// the VK of the physical digit-row key is its digit on every layout, while JUCE's key code is the character it types.
inline juce::String tokenForVirtualKey (int vk)
{
    if (vk < 0x20 || vk > 0x87)
        return {};
    juce::String candidate;
    if ((vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A))  candidate = juce::String::charToString ((juce::juce_wchar) vk);
    else if (vk >= 0x60 && vk <= 0x69)                              candidate = "Num" + juce::String (vk - 0x60);
    else if (vk >= 0x70 && vk <= 0x87)                              candidate = "F" + juce::String (vk - 0x70 + 1);
    else
    {
        int count = 0;
        for (const auto* k = detail::namedKeys (count); k != detail::namedKeys (count) + count; ++k)
            if (k->vk == vk)
                candidate = k->token;
    }
    return candidate;
}

// JUCE key code -> canonical token; empty for modifier-only and unsupported keys.
inline juce::String keyTokenFor (int code)
{
    using juce::KeyPress;
    if (code > 0 && code < 128)
    {
        const auto c = juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) code);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return juce::String::charToString (c);
    }
    if (code >= KeyPress::F1Key && code <= KeyPress::F24Key)       return "F" + juce::String (code - KeyPress::F1Key + 1);
    if (code >= KeyPress::numberPad0 && code <= KeyPress::numberPad9) return "Num" + juce::String (code - KeyPress::numberPad0);
    if (code == KeyPress::spaceKey)    return "Space";
    if (code == KeyPress::insertKey)   return "Insert";
    if (code == KeyPress::deleteKey)   return "Delete";
    if (code == KeyPress::homeKey)     return "Home";
    if (code == KeyPress::endKey)      return "End";
    if (code == KeyPress::pageUpKey)   return "PageUp";
    if (code == KeyPress::pageDownKey) return "PageDown";
    if (code == KeyPress::upKey)       return "Up";
    if (code == KeyPress::downKey)     return "Down";
    if (code == KeyPress::leftKey)     return "Left";
    if (code == KeyPress::rightKey)    return "Right";
    return {};
}

// The combination a key press stands for (the Win key is not part of a JUCE key press, so the caller passes it).
// `vkToken` is the key read from its virtual-key code (tokenForVirtualKey), which wins over JUCE's character-based
// code; empty falls back to the JUCE code. nullopt for an unsupported key; the result may have no modifier (the
// capture dialog tells the user).
inline std::optional<Combo> fromKeyPress (const juce::KeyPress& key, bool winDown, const juce::String& vkToken = {})
{
    const auto token = vkToken.isNotEmpty() ? vkToken : keyTokenFor (key.getKeyCode());
    if (token.isEmpty())
        return std::nullopt;

    Combo c;
    c.ctrl  = key.getModifiers().isCtrlDown();
    c.alt   = key.getModifiers().isAltDown();
    c.shift = key.getModifiers().isShiftDown();
    c.win   = winDown;
    c.key   = token;
    return c;
}

// RegisterHotKey arguments (MOD_NOREPEAT included: a held key fires once).
inline Win32Args toWin32 (const Combo& c)
{
    Win32Args a;
    a.vk = (unsigned) detail::virtualKeyFor (detail::canonicalKey (c.key));
    a.mods = (c.alt ? modAlt : 0u) | (c.ctrl ? modControl : 0u) | (c.shift ? modShift : 0u) | (c.win ? modWin : 0u) | modNoRepeat;
    if (a.vk == 0 || ! c.hasModifier())
        return {};
    return a;
}

// Index of the binding (one canonical text per preset, "" = none) that already holds `c`; -1 when it is free.
inline int findOwner (const juce::StringArray& bindings, const Combo& c)
{
    for (int i = 0; i < bindings.size(); ++i)
        if (const auto b = parse (bindings[i]); b.has_value() && *b == c)
            return i;
    return -1;
}

} // namespace lighthost::hotkeys
