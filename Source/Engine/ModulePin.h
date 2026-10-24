//
//  ModulePin.h — keep a plugin's binary mapped once it has been used in this process (Windows).
//
//  Some plugins start threads of their own (licence checks, telemetry) that outlive the plugin object. When the
//  last instance goes away JUCE unloads the module, and such a thread then runs in unmapped memory: the app
//  dies later, somewhere else (seen with ShapeIt and Voloco, 2026-10-24). Pinning the module with
//  GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_PIN) makes FreeLibrary a no-op for it until the process exits.
//
//  Only plugins that were really instantiated here are pinned, once per path: scanning happens in worker
//  processes, so a scanned-only plugin never touches this process. Cost: a plugin the user has used stays mapped
//  until exit. Other platforms: no-ops.
//

#pragma once

#include <JuceHeader.h>
#include <mutex>
#include <set>

namespace lighthost::modules
{
    // The binary a plugin path stands for: a single-file .vst3 is itself, a bundle folder holds
    // Contents\x86_64-win\<same name>. Pure; the file need not exist.
    inline juce::File binaryOfPlugin (const juce::File& pluginPath)
    {
        if (pluginPath.isDirectory())
            return pluginPath.getChildFile ("Contents").getChildFile ("x86_64-win").getChildFile (pluginPath.getFileName());

        return pluginPath;
    }

    // Once-per-path bookkeeping (shared by the pin itself and the tests).
    class PinnedSet
    {
    public:
        // true the first time a path is seen.
        bool insert (const juce::File& f)
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return paths.insert (f.getFullPathName().toLowerCase()).second;
        }

        void forget (const juce::File& f)
        {
            const std::lock_guard<std::mutex> lock { mutex };
            paths.erase (f.getFullPathName().toLowerCase());
        }

        bool contains (const juce::File& f) const
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return paths.count (f.getFullPathName().toLowerCase()) != 0;
        }

    private:
        mutable std::mutex mutex;
        std::set<juce::String> paths;
    };

    inline PinnedSet& pinnedSet()
    {
        static PinnedSet set;
        return set;
    }

    // Windows only (ModulePin.cpp). Other platforms: always false.
    bool isModuleLoaded (const juce::File& binary);

    // Pins the module of `pluginPath` (single file or bundle folder), once per path. True when it pinned
    // something now; false when it was pinned before, is not loaded, or on other platforms.
    bool pinPluginModule (const juce::File& pluginPath);
}
