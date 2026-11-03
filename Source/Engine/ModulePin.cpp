#include "ModulePin.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <tlhelp32.h>
#endif

using namespace juce;

namespace lighthost::modules
{
   #if JUCE_WINDOWS
    namespace
    {
        // The loaded module whose file is `binary`, or one inside the bundle folder `pluginPath` when the binary's
        // name differs (a bundle may name its binary otherwise). nullptr when nothing matches.
        HMODULE findLoadedModule (const File& pluginPath, const File& binary)
        {
            if (auto h = GetModuleHandleW (binary.getFullPathName().toWideCharPointer()))
                return h;

            const auto snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
            if (snapshot == INVALID_HANDLE_VALUE)
                return nullptr;

            HMODULE found = nullptr;
            MODULEENTRY32W entry {};
            entry.dwSize = sizeof (entry);

            for (BOOL ok = Module32FirstW (snapshot, &entry); ok && found == nullptr; ok = Module32NextW (snapshot, &entry))
            {
                const File module { String (entry.szExePath) };
                if (module == binary || (pluginPath.isDirectory() && module.isAChildOf (pluginPath)))
                    found = entry.hModule;
            }

            CloseHandle (snapshot);
            return found;
        }
    }

    bool isModuleLoaded (const File& binary)
    {
        return GetModuleHandleW (binary.getFullPathName().toWideCharPointer()) != nullptr;
    }

    bool pinPluginModule (const File& pluginPath)
    {
        if (! pinnedSet().insert (pluginPath))
            return false;

        if (const auto module = findLoadedModule (pluginPath, binaryOfPlugin (pluginPath)))
        {
            HMODULE pinned = nullptr;
            if (GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                    reinterpret_cast<LPCWSTR> (module), &pinned))
                return true;
        }

        pinnedSet().forget (pluginPath);   // not loaded (a stand-in, a test double): try again next time
        return false;
    }
   #else
    bool isModuleLoaded (const File&)       { return false; }
    bool pinPluginModule (const File&)      { return false; }
   #endif
}
