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

    // Copies the settings file byte for byte to <settings file name>-<label>-<yyyyMMdd-HHmmss>.
    // Returns the copy, or {} when there is no file or the copy failed.
    inline juce::File copySettingsFileAside (const juce::PropertiesFile& settings, const juce::String& label)
    {
        const auto file = settings.getFile();
        if (! file.existsAsFile())
            return {};

        const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
        const auto copy = file.getSiblingFile (file.getFileName() + "-" + label + "-" + stamp);
        return file.copyFileTo (copy) ? copy : juce::File();
    }

    // The whole settings file is unreadable (not well-formed XML), so PropertiesFile loaded nothing and the
    // next save would overwrite it with defaults. Copies the file byte for byte to
    //   <settings file name>-unreadable-<yyyyMMdd-HHmmss>
    // nullopt = file absent or readable (nothing to do); File{} = unreadable but the copy failed; else the copy.
    inline std::optional<juce::File> backupUnreadableSettingsFile (juce::PropertiesFile& settings)
    {
        if (! settings.getFile().existsAsFile() || settings.isValidFile())
            return std::nullopt;

        return copySettingsFileAside (settings, "unreadable");
    }
}
