//
//  SettingsBackup.h — keep a copy of a settings value that could not be parsed, so the
//  next save (which writes the fallback defaults) can never destroy the user's data.
//  Never deletes anything.
//

#pragma once

#include <JuceHeader.h>

namespace lighthost
{
    // Writes `raw` to a sibling of the settings file,
    //   <settings name>.<key>-unreadable-<yyyyMMdd-HHmmss>.xml
    // and stores it under the settings key "<key>.unreadable" (latest only).
    // Returns the file, or {} when it could not be written.
    inline juce::File backupUnreadable (juce::PropertiesFile& settings, const juce::String& key, const juce::String& raw)
    {
        settings.setValue (key + ".unreadable", raw);

        const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
        const auto settingsFile = settings.getFile();
        const auto backup = settingsFile.getSiblingFile (settingsFile.getFileNameWithoutExtension()
                                                         + "." + key + "-unreadable-" + stamp + ".xml");

        if (! backup.replaceWithText (raw))
            return {};

        return backup;
    }
}
