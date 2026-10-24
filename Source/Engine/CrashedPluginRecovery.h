//
//  CrashedPluginRecovery.h — turn the plugin scan's dead man's pedal into a persistent blacklist.
//
//  JUCE writes the file being scanned to <pedal> (and, mid-write, to a hidden sibling
//  ".<pedal>_temp<hex>") and removes it again when the scan returns. After a crash the file
//  names the culprit. JUCE's PluginListComponent only copies it into the in-memory blacklist and
//  deletes it, so a second crash before the list is saved forgets it, and a crash inside the
//  pedal's own temp-file replace leaves it where JUCE never looks. This reads both, and the
//  caller saves the blacklist BEFORE calling discardCrashedPluginRecords().
//

#pragma once

#include <JuceHeader.h>

namespace lighthost::scan
{
    // "<settings file stem>.<suffix>" next to the settings file, so that every instance (-multi-instance=<name>) keeps
    // its own crash records: "Light Host.RecentlyCrashedPluginsList", "Light Host.LoadingPlugin", ...
    inline juce::File perInstanceFile (const juce::File& settingsFile, const juce::String& suffix)
    {
        return settingsFile.getSiblingFile (settingsFile.getFileNameWithoutExtension() + "." + suffix);
    }

    // The fixed name of v1.2 before the per-instance names; still read (and deleted) once.
    inline juce::File legacyPedal (const juce::File& pedal)
    {
        return pedal.getSiblingFile ("RecentlyCrashedPluginsList");
    }

    inline void addRecordFiles (juce::Array<juce::File>& files, const juce::File& pedal)
    {
        if (pedal.existsAsFile())
            files.addIfNotAlreadyThere (pedal);

        const auto name = pedal.getFileNameWithoutExtension();
        for (const auto& f : pedal.getParentDirectory().findChildFiles (juce::File::findFiles, false, "." + name + "_temp*"))
            files.addIfNotAlreadyThere (f);
    }

    // The pedal file and every leftover temp file next to it, plus the same for the legacy fixed name.
    inline juce::Array<juce::File> crashedPluginRecordFiles (const juce::File& pedal)
    {
        juce::Array<juce::File> files;
        if (pedal == juce::File())
            return files;

        addRecordFiles (files, pedal);
        addRecordFiles (files, legacyPedal (pedal));
        return files;
    }

    // Paths named by those files, in file order, without empty lines or duplicates.
    // readLines understands JUCE's UTF-16-with-BOM output as well as plain text.
    inline juce::StringArray readCrashedPlugins (const juce::File& pedal)
    {
        juce::StringArray paths;
        for (const auto& f : crashedPluginRecordFiles (pedal))
        {
            juce::StringArray lines;
            f.readLines (lines);
            for (const auto& l : lines)
                if (l.trim().isNotEmpty())
                    paths.addIfNotAlreadyThere (l.trim());
        }
        return paths;
    }

    // Deletes the records. Call only after the blacklist has been saved.
    inline void discardCrashedPluginRecords (const juce::File& pedal)
    {
        for (const auto& f : crashedPluginRecordFiles (pedal))
            f.deleteFile();
    }

    // Blacklists every crashed path that is not on the list yet; returns those (for the notice).
    inline juce::StringArray blacklistCrashedPlugins (juce::KnownPluginList& list, const juce::StringArray& crashed)
    {
        juce::StringArray added;
        for (const auto& p : crashed)
            if (! list.getBlacklistedFiles().contains (p))
            {
                list.addToBlacklist (p);
                added.add (p);
            }
        return added;
    }

    // "Plugin scan crashed on A, B; it will be skipped. Scan again to continue." ({} if none)
    inline juce::String crashedPluginsNotice (const juce::StringArray& paths)
    {
        if (paths.isEmpty())
            return {};

        juce::StringArray names;
        for (const auto& p : paths)
            names.add (juce::File::createFileWithoutCheckingPath (p).getFileNameWithoutExtension());

        return "Plugin scan crashed on " + names.joinIntoString (", ") + "; it will be skipped. Scan again to continue.";
    }
}
