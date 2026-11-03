#include "PresetHotkeys.h"
#include "Engine/HotkeyCombo.h"

#if JUCE_WINDOWS

#include <windows.h>

namespace
{
    const wchar_t* const windowClassName = L"LightHostPresetHotkeys";
}

struct PresetHotkeys::Impl
{
    explicit Impl (PresetHotkeys& o) : owner (o) {}

    ~Impl()
    {
        *alive = false;   // a press already queued must not reach a destroyed owner
        destroyWindow();
    }

    bool ensureWindow()
    {
        if (hwnd != nullptr)
            return true;

        const auto instance = GetModuleHandleW (nullptr);
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof (wc);
        wc.lpfnWndProc = &Impl::windowProc;
        wc.hInstance = instance;
        wc.lpszClassName = windowClassName;
        RegisterClassExW (&wc);   // already registered is fine

        hwnd = CreateWindowExW (0, windowClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
        return hwnd != nullptr;
    }

    void unregisterAll()
    {
        if (hwnd != nullptr)
            for (int i = 0; i < registered.size(); ++i)
                UnregisterHotKey (hwnd, i + 1);
        registered.clear();
    }

    void destroyWindow()
    {
        unregisterAll();
        if (hwnd != nullptr)
        {
            DestroyWindow (hwnd);
            hwnd = nullptr;
            UnregisterClassW (windowClassName, GetModuleHandleW (nullptr));
        }
    }

    static LRESULT CALLBACK windowProc (HWND h, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
        {
            SetWindowLongPtrW (h, GWLP_USERDATA, (LONG_PTR) ((CREATESTRUCTW*) lParam)->lpCreateParams);
        }
        else if (msg == WM_HOTKEY)
        {
            if (auto* self = (Impl*) GetWindowLongPtrW (h, GWLP_USERDATA))
            {
                const int slot = (int) wParam - 1;
                if (slot >= 0 && slot < self->registered.size())
                {
                    // Never switch inside the window procedure: a plugin dialog during a load runs a nested
                    // message loop, and handling a second press there would edit a half-built graph.
                    juce::MessageManager::callAsync ([alive = self->alive, self, combo = self->registered[slot]]
                    {
                        if (*alive && self->owner.onPressed)
                            self->owner.onPressed (combo);
                    });
                }
            }
            return 0;
        }
        return DefWindowProcW (h, msg, wParam, lParam);
    }

    PresetHotkeys& owner;
    HWND hwnd = nullptr;
    juce::StringArray registered;   // slot i is registered with id i + 1
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};

PresetHotkeys::PresetHotkeys() : impl (std::make_unique<Impl> (*this)) {}
PresetHotkeys::~PresetHotkeys() = default;

juce::StringArray PresetHotkeys::sync (const juce::StringArray& combos)
{
    juce::StringArray failed;
    if (! juce::MessageManager::existsAndIsCurrentThread())
        return combos;   // the window must be made on the message thread

    impl->unregisterAll();
    if (combos.isEmpty() || ! impl->ensureWindow())
    {
        impl->destroyWindow();
        return combos;   // empty: nothing failed; otherwise the window could not be created
    }

    for (const auto& text : combos)
    {
        const auto combo = lighthost::hotkeys::parse (text);
        const auto args = combo.has_value() ? lighthost::hotkeys::toWin32 (*combo) : lighthost::hotkeys::Win32Args {};
        const int id = impl->registered.size() + 1;
        if (args.vk != 0 && RegisterHotKey (impl->hwnd, id, args.mods, args.vk))
            impl->registered.add (text);
        else
            failed.add (text);
    }

    if (impl->registered.isEmpty())
        impl->destroyWindow();   // nothing registered: no window either
    return failed;
}

juce::String PresetHotkeys::pressedKeyToken()
{
    for (int vk = 0x20; vk <= 0x87; ++vk)
        if ((GetAsyncKeyState (vk) & 0x8000) != 0)
            if (const auto token = lighthost::hotkeys::tokenForVirtualKey (vk); token.isNotEmpty())
                return token;
    return {};
}

bool PresetHotkeys::isWinKeyDown()
{
    return (GetAsyncKeyState (VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState (VK_RWIN) & 0x8000) != 0;
}

#else

struct PresetHotkeys::Impl {};
PresetHotkeys::PresetHotkeys() = default;
PresetHotkeys::~PresetHotkeys() = default;
juce::StringArray PresetHotkeys::sync (const juce::StringArray& combos) { return combos; }
juce::String PresetHotkeys::pressedKeyToken() { return {}; }
bool PresetHotkeys::isWinKeyDown() { return false; }

#endif
