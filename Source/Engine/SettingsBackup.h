//
//  SettingsBackup.h — keep a copy of a settings value that could not be parsed, so the
//  next save (which writes the fallback defaults) can never destroy the user's data.
//  Never deletes anything.
//

#pragma once

#include <JuceHeader.h>
#include <optional>

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

    // The whole settings file is unreadable (not well-formed XML), so PropertiesFile loaded nothing and the
    // next save would overwrite it with defaults. Copies the file byte for byte to
    //   <settings file name>-unreadable-<yyyyMMdd-HHmmss>
    // nullopt = file absent or readable (nothing to do); File{} = unreadable but the copy failed; else the copy.
    inline std::optional<juce::File> backupUnreadableSettingsFile (juce::PropertiesFile& settings)
    {
        const auto file = settings.getFile();
        if (! file.existsAsFile() || settings.isValidFile())
            return std::nullopt;

        const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
        const auto copy = file.getSiblingFile (file.getFileName() + "-unreadable-" + stamp);

        if (! file.copyFileTo (copy))
            return juce::File();

        return copy;
    }
}
