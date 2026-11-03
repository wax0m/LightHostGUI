//
//  LoadGuard.h — survive a plugin that kills the process while it loads.
//
//  Before a plugin is instantiated in the main process, its file and name are written to the guard file
//  (<settings stem>.LoadingPlugin). The file is removed when the gesture that loaded it has finished, so
//  a file that is still there at the next start means the last run died inside that plugin. The plugin's
//  file then goes on the persistent suspended list (settings key "suspendedPlugins"): every node that uses
//  it, in the live graph and in every preset, loads as a stand-in without instantiation until an explicit
//  retry succeeds. Pure helpers, no GraphController dependency.
//

#pragma once

#include <JuceHeader.h>
#include <optional>

namespace lighthost::guard
{
    struct Record
    {
        juce::String path, name;
    };

    // One small UTF-8 file, overwritten per plugin: line 1 the plugin file, line 2 its name.
    inline bool writeGuard (const juce::File& file, const juce::String& path, const juce::String& name)
    {
        if (file == juce::File())
            return false;
        return file.replaceWithText (path.replaceCharacter ('\n', ' ') + "\n" + name.replaceCharacter ('\n', ' ') + "\n",
                                     false, false, "\n");
    }

    inline std::optional<Record> readGuard (const juce::File& file)
    {
        if (file == juce::File() || ! file.existsAsFile())
            return std::nullopt;

        juce::StringArray lines;
        file.readLines (lines);
        if (lines.isEmpty() || lines[0].trim().isEmpty())
            return std::nullopt;   // empty or damaged: nothing trustworthy to suspend

        const auto path = lines[0].trim();
        const auto name = lines.size() > 1 && lines[1].trim().isNotEmpty()
                              ? lines[1].trim()
                              : juce::File::createFileWithoutCheckingPath (path).getFileNameWithoutExtension();
        return Record { path, name };
    }

    inline void clearGuard (const juce::File& file)
    {
        if (file != juce::File())
            file.deleteFile();
    }

    // The suspended list is stored as one settings value, a path per line.
    inline juce::StringArray suspendedFromString (const juce::String& value)
    {
        juce::StringArray out;
        for (const auto& l : juce::StringArray::fromLines (value))
            if (l.trim().isNotEmpty())
                out.addIfNotAlreadyThere (l.trim());
        return out;
    }

    inline juce::String suspendedToString (const juce::StringArray& files)
    {
        return files.joinIntoString ("\n");
    }

    // Plugin files compare case-insensitively (Windows paths).
    inline bool isSuspended (const juce::StringArray& suspended, const juce::String& file)
    {
        return file.isNotEmpty() && suspended.contains (file, true);
    }

    // Safe mode: hold Shift while LightHost starts, or pass -safe-mode. Every plugin node then loads as a stand-in.
    inline bool wantsSafeMode (const juce::StringArray& commandLine, bool shiftDown)
    {
        return shiftDown || commandLine.contains ("-safe-mode", true);
    }

    inline juce::String safeModeNotice()
    {
        return "Safe mode: plugins are not loaded. Restart normally to load them.";
    }

    inline juce::String loadGuardNotice (const juce::String& name)
    {
        return "LightHost closed while loading " + name + ". It now runs as a stand-in: remove it, or double-click to retry.";
    }
}
