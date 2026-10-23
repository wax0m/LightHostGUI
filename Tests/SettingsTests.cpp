//
//  SettingsTests.cpp — headless tests for the whole-file settings backup
//  (backupUnreadableSettingsFile): an unreadable file is copied aside, anything else is left alone.
//

#include "../Source/Engine/SettingsBackup.h"
#include <iostream>

using namespace juce;

namespace
{
    int failures = 0;
    void check (int& checks, bool cond, const String& msg)
    {
        ++checks;
        if (! cond) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; }
    }

    PropertiesFile::Options optionsFor (const File& file)
    {
        PropertiesFile::Options o;
        o.filenameSuffix = file.getFileExtension();
        return o;
    }

    int siblingCount (const File& dir)
    {
        return dir.getNumberOfChildFiles (File::findFiles, "*");
    }
}

int runSettingsTests (int& checks)
{
    failures = 0;

    const auto dir = File::getSpecialLocation (File::tempDirectory)
                         .getChildFile ("lighthost-settings-test-" + String::toHexString (Random::getSystemRandom().nextInt64()));
    dir.createDirectory();

    // (a) garbage file: copied byte for byte, original untouched
    {
        const auto file = dir.getChildFile ("garbage.settings");
        file.replaceWithText ("<<< this is not xml");
        const auto before = file.loadFileAsString();

        PropertiesFile props (file, optionsFor (file));
        const auto result = lighthost::backupUnreadableSettingsFile (props);

        check (checks, result.has_value(), "unreadable file: a result is returned");
        check (checks, result.has_value() && *result != File(), "unreadable file: the copy succeeded");
        check (checks, result.has_value() && result->existsAsFile(), "unreadable file: the copy exists");
        check (checks, result.has_value() && result->hasIdenticalContentTo (file), "unreadable file: the copy is identical");
        check (checks, result.has_value() && result->getFileName().startsWith ("garbage.settings-unreadable-"),
               "unreadable file: copy name is <name>-unreadable-<stamp>");
        check (checks, file.loadFileAsString() == before, "unreadable file: the original is untouched");
    }

    // (b) valid settings file: nothing to do, no sibling created
    {
        const auto file = dir.getChildFile ("valid.settings");
        {
            PropertiesFile writer (file, optionsFor (file));
            writer.setValue ("k", "v");
            writer.saveIfNeeded();
        }
        const auto before = siblingCount (dir);

        PropertiesFile props (file, optionsFor (file));
        check (checks, ! lighthost::backupUnreadableSettingsFile (props).has_value(), "valid file: nullopt");
        check (checks, siblingCount (dir) == before, "valid file: no sibling created");
    }

    // (c) missing file: nothing to do
    {
        const auto file = dir.getChildFile ("missing.settings");
        const auto before = siblingCount (dir);

        PropertiesFile props (file, optionsFor (file));
        check (checks, ! lighthost::backupUnreadableSettingsFile (props).has_value(), "missing file: nullopt");
        check (checks, siblingCount (dir) == before, "missing file: no sibling created");
        check (checks, ! file.exists(), "missing file: nothing created");
    }

    dir.deleteRecursively();
    return failures;
}
