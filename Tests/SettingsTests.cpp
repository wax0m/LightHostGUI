//
//  SettingsTests.cpp — headless tests for the whole-file settings backup
//  (backupUnreadableSettingsFile): an unreadable file is copied aside, anything else is left alone.
//

#include "../Source/Engine/SettingsBackup.h"
#include "../Source/Engine/CrashedPluginRecovery.h"
#include "../Source/Engine/LoadGuard.h"
#include "../Source/Engine/PluginChoice.h"
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

    // (d) crashed-plugin recovery: pedal file + leftover temp file -> blacklist, then discard
    {
        const auto pedal = dir.getChildFile ("RecentlyCrashedPluginsList");
        const String a = "C:\\VST3\\Steinberg\\HALion Sonic.vst3", b = "C:\\VST3\\Other.vst3";

        check (checks, lighthost::scan::readCrashedPlugins (pedal).isEmpty(), "pedal: absent file reads as empty");
        check (checks, lighthost::scan::crashedPluginsNotice ({}).isEmpty(), "pedal: no notice for an empty list");

        // JUCE's format: UTF-16 with a BOM, '\n'-separated; a BOM-only file is an empty list.
        pedal.replaceWithText ("", true, true);
        check (checks, lighthost::scan::readCrashedPlugins (pedal).isEmpty(), "pedal: BOM-only file is an empty list");

        // The crash left only the hidden temp file (the replace never landed).
        dir.getChildFile (".RecentlyCrashedPluginsList_temp2bafcced").replaceWithText (a, true, true);
        auto crashed = lighthost::scan::readCrashedPlugins (pedal);
        check (checks, crashed.size() == 1 && crashed[0] == a, "pedal: a leftover temp file is read even when the pedal is empty");

        pedal.replaceWithText (b + "\n" + a + "\n\n", true, true);   // duplicate across both files, blank lines
        crashed = lighthost::scan::readCrashedPlugins (pedal);
        check (checks, crashed.size() == 2 && crashed.contains (a) && crashed.contains (b), "pedal: both files merged, no duplicates");

        KnownPluginList list;
        list.addToBlacklist (b);
        const auto added = lighthost::scan::blacklistCrashedPlugins (list, crashed);
        check (checks, added.size() == 1 && added[0] == a, "blacklist: only the newly blacklisted path is returned");
        check (checks, list.getBlacklistedFiles().contains (a) && list.getBlacklistedFiles().contains (b), "blacklist: both paths are on the list");
        check (checks, lighthost::scan::blacklistCrashedPlugins (list, crashed).isEmpty(), "blacklist: a second pass adds nothing");

        // The blacklist survives a save / load round trip (this is what the app persists before discarding).
        KnownPluginList reloaded;
        if (auto xml = list.createXml())
            reloaded.recreateFromXml (*xml);
        check (checks, reloaded.getBlacklistedFiles().contains (a), "blacklist: HALion survives createXml / recreateFromXml");

        check (checks, lighthost::scan::crashedPluginsNotice ({ a }) == "Plugin scan crashed on HALion Sonic; it will be skipped. Scan again to continue.",
               "notice: wording and file name without extension");
        check (checks, lighthost::scan::crashedPluginsNotice ({ a, b }).contains ("HALion Sonic, Other"), "notice: several names are joined");

        lighthost::scan::discardCrashedPluginRecords (pedal);
        check (checks, ! pedal.exists() && lighthost::scan::crashedPluginRecordFiles (pedal).isEmpty(), "discard: pedal and temp files are gone");
        check (checks, lighthost::scan::readCrashedPlugins (pedal).isEmpty(), "discard: nothing is read afterwards");
    }

    // (e) per-instance names; the legacy fixed pedal is read and deleted once
    {
        const auto def = lighthost::scan::perInstanceFile (dir.getChildFile ("Light Host.settings"), "RecentlyCrashedPluginsList");
        const auto multi = lighthost::scan::perInstanceFile (dir.getChildFile ("Light Host.test.settings"), "RecentlyCrashedPluginsList");
        const auto guard = lighthost::scan::perInstanceFile (dir.getChildFile ("Light Host.test.settings"), "LoadingPlugin");
        check (checks, def.getFileName() == "Light Host.RecentlyCrashedPluginsList", "names: default instance pedal");
        check (checks, multi.getFileName() == "Light Host.test.RecentlyCrashedPluginsList", "names: multi-instance pedal differs");
        check (checks, guard.getFileName() == "Light Host.test.LoadingPlugin", "names: load guard file");

        const String a = "C:/VST3/Legacy.vst3", b = "C:/VST3/Mine.vst3";
        dir.getChildFile ("RecentlyCrashedPluginsList").replaceWithText (a, true, true);
        def.replaceWithText (b, true, true);
        auto crashed = lighthost::scan::readCrashedPlugins (def);
        check (checks, crashed.size() == 2 && crashed.contains (a) && crashed.contains (b), "legacy: old fixed pedal is read with the new one");
        lighthost::scan::discardCrashedPluginRecords (def);
        check (checks, ! dir.getChildFile ("RecentlyCrashedPluginsList").exists() && ! def.exists(), "legacy: both are deleted");
        check (checks, lighthost::scan::readCrashedPlugins (def).isEmpty(), "legacy: nothing is read twice");
    }

    // (f) load guard files and the persistent suspended list
    {
        const auto gfile = lighthost::scan::perInstanceFile (dir.getChildFile ("Light Host.settings"), "LoadingPlugin");
        const String path = "C:/VST3/Steinberg/Groove Agent SE.vst3", name = "Groove Agent SE";
        check (checks, ! lighthost::guard::readGuard (gfile).has_value(), "guard: absent file reads as none");

        check (checks, lighthost::guard::writeGuard (gfile, path, name), "guard: written");
        auto rec = lighthost::guard::readGuard (gfile);
        check (checks, rec.has_value() && rec->path == path && rec->name == name, "guard: path and name round-trip");

        lighthost::guard::writeGuard (gfile, "C:/VST3/Other.vst3", "Other");
        rec = lighthost::guard::readGuard (gfile);
        check (checks, rec.has_value() && rec->path == "C:/VST3/Other.vst3", "guard: the next write replaces the record");

        gfile.replaceWithText ("");
        check (checks, ! lighthost::guard::readGuard (gfile).has_value(), "guard: an empty file is not trusted");
        gfile.replaceWithText (dir.getChildFile ("Groove Agent SE.vst3").getFullPathName() + String::charToString (10));
        rec = lighthost::guard::readGuard (gfile);
        check (checks, rec.has_value() && rec->name == "Groove Agent SE", "guard: a missing name falls back to the file name");

        lighthost::guard::clearGuard (gfile);
        check (checks, ! gfile.exists() && ! lighthost::guard::readGuard (gfile).has_value(), "guard: cleared");
        check (checks, lighthost::guard::loadGuardNotice (name).startsWith ("LightHost closed while loading Groove Agent SE."),
               "guard: notice wording");

        // The suspended list survives a real settings save / load.
        StringArray list { path, "C:/VST3/Other.vst3" };
        {
            const auto file = dir.getChildFile ("susp.settings");
            {
                PropertiesFile props (file, optionsFor (file));
                props.setValue ("suspendedPlugins", lighthost::guard::suspendedToString (list));
                props.saveIfNeeded();
            }
            PropertiesFile props (file, optionsFor (file));
            const auto loaded = lighthost::guard::suspendedFromString (props.getValue ("suspendedPlugins"));
            check (checks, loaded == list, "suspended: list survives save and load");
            check (checks, lighthost::guard::isSuspended (loaded, "c:/vst3/steinberg/groove agent se.vst3"), "suspended: case-insensitive match");
            check (checks, ! lighthost::guard::isSuspended (loaded, "C:/VST3/Third.vst3") && ! lighthost::guard::isSuspended (loaded, ""),
                   "suspended: other files and empty names do not match");
        }
        check (checks, lighthost::guard::suspendedFromString ("").isEmpty(), "suspended: empty value is an empty list");

        check (checks, lighthost::guard::wantsSafeMode ({}, true) && lighthost::guard::wantsSafeMode (StringArray { "-safe-mode" }, false),
               "safe mode: Shift or -safe-mode");
        check (checks, ! lighthost::guard::wantsSafeMode (StringArray { "-multi-instance=x" }, false), "safe mode: off by default");

        // loadablePlugins: a suspended file is filtered out, its description stays in the stored list.
        Array<PluginDescription> known;
        PluginDescription a, b;
        a.name = "Groove Agent SE"; a.pluginFormatName = "VST3"; a.fileOrIdentifier = path;
        b.name = "Other"; b.pluginFormatName = "VST3"; b.fileOrIdentifier = "C:/VST3/Third.vst3";
        known.add (a); known.add (b);
        const auto offered = lighthost::choice::filterLoadable (known, StringArray { "VST3" }, StringArray (path));
        check (checks, offered.size() == 1 && offered[0].name == "Other" && known.size() == 2, "loadable: a suspended file is not offered");
        check (checks, lighthost::choice::filterLoadable (known, StringArray { "VST3" }).size() == 2, "loadable: nothing suspended, nothing hidden");

        // A scan-blacklisted file keeps its old description in the list (JUCE's addToBlacklist doesn't remove it).
        {
            KnownPluginList scanned;
            scanned.addType (a);
            scanned.addType (b);
            scanned.addToBlacklist (path);
            StringArray hidden;
            hidden.addArray (scanned.getBlacklistedFiles());
            const auto shown = lighthost::choice::filterLoadable (scanned.getTypes(), StringArray { "VST3" }, hidden);
            check (checks, scanned.getNumTypes() == 2 && shown.size() == 1 && shown[0].name == "Other",
                   "loadable: a blacklisted file is not offered, its description stays in the list");

            hidden.add ("C:/VST3/Third.vst3");
            check (checks, lighthost::choice::filterLoadable (scanned.getTypes(), StringArray { "VST3" }, hidden).isEmpty(),
                   "loadable: blacklisted and suspended files are both hidden");
        }
    }

    dir.deleteRecursively();
    return failures;
}
