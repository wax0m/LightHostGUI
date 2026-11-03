//
//  IconMenu.cpp
//  Light Host
//
//  Created by Rolando Islas on 12/26/15.
//  Ported to JUCE 8, 2026. Milestone 2: GraphDocument/GraphController replace
//  the legacy timestamp-ordered PropertiesFile chain.
//

#include <JuceHeader.h>
#include "IconMenu.hpp"
#include "PluginWindow.h"
#include "Engine/SettingsBackup.h"
#include "Engine/CrashedPluginRecovery.h"
#include "Engine/PluginScanWorker.h"
#include "Engine/MidiInputPlan.h"
#include "UI/MainWindow.h"
#include "Engine/PluginChoice.h"
#include "Autostart.h"
#include "UI/ShortcutCapture.h"
#if JUCE_WINDOWS
#include "Windows.h"
#endif

// Left-click menu id for "Show Editor Window". Safe: the fixed items use 1/2,
// the preset items use INDEX_PRESET (>= 6,000,000), and KnownPluginList's
// available-plugins ids start at 0x324503f4 — nothing else claims 3.
static constexpr int kShowWindowMenuId = 3;
static constexpr int kMonoInputMenuId  = 6;   // checkable "Mono Input" toggle
static constexpr int kRetryMissingMenuId = 7; // "Retry missing plugins (N)", shown only when N > 0
static constexpr int kStartWithWindowsMenuId = 8; // checkable "Start with Windows" (Windows, not in -multi-instance)

class IconMenu::PluginListWindow : public DocumentWindow
{
public:
    PluginListWindow (IconMenu& owner_, AudioPluginFormatManager& pluginFormatManager)
        : DocumentWindow ("Available Plugins", Colours::white,
            DocumentWindow::minimiseButton | DocumentWindow::closeButton),
        owner (owner_)
    {
        // JUCE's constructor below moves the pedal into the in-memory blacklist and deletes it, so
        // anything a crash left behind has to be blacklisted and saved before that happens.
        owner.recoverCrashedPlugins();

        const File deadMansPedalFile (IconMenu::crashedPluginsPedal());

        auto* const list = new PluginListComponent (pluginFormatManager,
            owner.knownPluginList,
            deadMansPedalFile,
            getAppProperties().getUserSettings());
        list->setNumberOfThreadsForScanning (1);   // the scan only waits for workers: keep the window alive meanwhile
        setContentOwned (list, true);

        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (300, 400, 800, 1500);
        setTopLeftPosition (60, 60);

        restoreWindowStateFromString (getAppProperties().getUserSettings()->getValue ("listWindowPos"));
        setVisible (true);
    }

    ~PluginListWindow() override
    {
        getAppProperties().getUserSettings()->setValue ("listWindowPos", getWindowStateAsString());

        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        owner.removePluginsLackingInputOutput();
        #if JUCE_MAC
        Process::setDockIconVisible (false);
        #endif
        owner.pluginListWindow = nullptr;
    }

private:
    IconMenu& owner;
};

// "-multi-instance=<name>" copies share the machine with the main one: no Start with Windows, no global shortcuts.
static bool runsAsMultiInstance()
{
    for (const auto& arg : JUCEApplication::getInstance()->getCommandLineParameterArray())
        if (arg.startsWith ("-multi-instance"))
            return true;
    return false;
}

namespace
{
    // Marks "a preset document is being loaded" for the lifetime of the scope.
    struct BusyScope
    {
        explicit BusyScope (bool& f) : flag (f) { flag = true; }
        ~BusyScope() { flag = false; }
        bool& flag;
    };
}

IconMenu::IconMenu (juce::String startupWarning, bool safeMode) : INDEX_PRESET (6000000)
{
    #if JUCE_WINDOWS
    shortcutsAllowed = ! safeMode && ! runsAsMultiInstance();
    #endif
    // Initialization
    addDefaultFormatsToManager (formatManager);   // JUCE 8: replaces AudioPluginFormatManager::addDefaultFormats()
    // Every plugin file is scanned in a child process of its own; a crash there blacklists that file only.
    knownPluginList.setCustomScanner (std::make_unique<lighthost::scan::OutOfProcessScanner>());
    #if JUCE_WINDOWS
    x = y = 0;
    #endif
    // Audio device
    auto savedAudioState = getAppProperties().getUserSettings()->getXmlValue ("audioDeviceState");
    deviceManager.initialise (256, 256, savedAudioState.get(), true);
    player.setProcessor (&graph);
    deviceManager.addAudioCallback (&player);
    // MIDI inputs -> player -> graph's midiInputNode (M7). The player is a
    // MidiInputCallback and forwards MIDI into the graph it drives.
    syncMidiInputs();
    deviceManager.addMidiInputDeviceCallback ({}, &player);   // {} = all enabled inputs
    // A keyboard switched on after LightHost started is opened without a restart; the callback fires only when the
    // system's device list changes (no timer).
    midiListConnection = MidiDeviceListConnection::make ([this] { syncMidiInputs(); });
    // Plugins - known list
    auto savedPluginList = getAppProperties().getUserSettings()->getXmlValue ("pluginList");
    if (savedPluginList != nullptr)
        knownPluginList.recreateFromXml (*savedPluginList);
    pluginSortMethod = KnownPluginList::sortByManufacturer;
    recoverCrashedPlugins();   // before any scan and before the change listener: saves the blacklist itself
    controller.setSafeMode (safeMode);
    if (safeMode)
        postNotice (lighthost::guard::safeModeNotice());
    recoverLoadGuard();   // a plugin that killed the last start becomes a stand-in before the graph is built
    knownPluginList.addChangeListener (this);
    // The device's output layout first, so the graph is built on the right pair straight away.
    controller.setOutputLayout (currentOutputLayout());
    deviceManager.addChangeListener (this);
    // Signal graph (migrates legacy chain settings on first run)
    controller.loadFrom (*getAppProperties().getUserSettings());
    // Mono-input preference (app-global; sums a mono source to both channels).
    controller.setMonoInput (getAppProperties().getUserSettings()->getBoolValue ("monoInput", false));
    // Presets: load the store, or seed a "Default" from the just-loaded graph.
    loadPresets();

    // A v1.0 build would read a v2 graph as px values clamped to 1 (every node bottom-right), so keep the
    // v1 file once. It has to happen BEFORE pinLegacyOutputs below: that call saves, and a copy taken after
    // it holds the migrated v2 graphs (and the pinned OUTs) instead of the v1 file. No dialog: the
    // migration itself is lossless.
    if (controller.didMigrateOnLoad() || store.migratedOnLoad)
        lighthost::copySettingsFileAside (*getAppProperties().getUserSettings(), "v1");

    pinLegacyOutputs (controller.didFixOnLoad());   // stored presets (and the saved graph, if loadFrom stamped it)
    syncHotkeys();

    // One non-modal notice for whatever could not be read (the backups are already written).
    const auto warning = (startupWarning + "\n\n" + controller.getLoadWarning() + "\n\n" + presetsWarning).trim();
    if (warning.isNotEmpty())
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Light Host", warning);

    setIcon();
    setIconTooltip (JUCEApplication::getInstance()->getApplicationName());
}

File IconMenu::crashedPluginsPedal()
{
    return lighthost::scan::perInstanceFile (getAppProperties().getUserSettings()->getFile(), "RecentlyCrashedPluginsList");
}

// A one-line notice: on the main window when it exists, otherwise kept for its first opening.
void IconMenu::postNotice (const String& notice)
{
    if (notice.isEmpty())
        return;

    if (mainWindow != nullptr)
        mainWindow->showNotice (notice);
    else
        startupNotice = startupNotice.isEmpty() ? notice : startupNotice + "  |  " + notice;
}

// Opens the MIDI inputs that are present and not open, closes the handle of one that went away (so that it is
// opened afresh when it comes back) and reports an input another app holds.
void IconMenu::syncMidiInputs()
{
    if (isShutDown)
        return;

    Array<lighthost::midi::Device> available;
    for (const auto& d : MidiInput::getAvailableDevices())
        available.add ({ d.name, d.identifier });

    StringArray enabledIds;
    for (const auto& d : available)
        if (deviceManager.isMidiInputDeviceEnabled (d.identifier))
            enabledIds.add (d.identifier);
    for (const auto& id : midiSeenIds)   // a gone device may still count as open
        if (deviceManager.isMidiInputDeviceEnabled (id))
            enabledIds.addIfNotAlreadyThere (id);

    const auto plan = lighthost::midi::planMidiInputs (available, midiSeenIds, enabledIds);

    for (const auto& id : plan.toDisable)
        deviceManager.setMidiInputDeviceEnabled (id, false);

    StringArray busy;
    for (const auto& d : plan.toEnable)
    {
        deviceManager.setMidiInputDeviceEnabled (d.identifier, false);   // drop a stale handle of the same id, if any
        deviceManager.setMidiInputDeviceEnabled (d.identifier, true);
        if (! deviceManager.isMidiInputDeviceEnabled (d.identifier))
            busy.add (d.name);
    }

    midiSeenIds.clear();
    for (const auto& d : available)
        midiSeenIds.add (d.identifier);

    postNotice (lighthost::midi::midiInUseNotice (busy));
}

// A plugin that kills the process during a scan is named by the dead man's pedal. Blacklist it, save the
// list (before the files are deleted, so another crash cannot lose it) and tell the user without a dialog.
void IconMenu::recoverCrashedPlugins()
{
    const auto pedal = crashedPluginsPedal();
    const auto crashed = lighthost::scan::readCrashedPlugins (pedal);
    if (crashed.isEmpty())
    {
        lighthost::scan::discardCrashedPluginRecords (pedal);   // BOM-only pedal and stray temp files
        return;
    }

    const auto added = lighthost::scan::blacklistCrashedPlugins (knownPluginList, crashed);

    if (auto xml = knownPluginList.createXml())
    {
        auto* const settings = getAppProperties().getUserSettings();
        settings->setValue ("pluginList", xml.get());
        if (! settings->saveIfNeeded())
            return;   // keep the records: the next start tries again
    }

    lighthost::scan::discardCrashedPluginRecords (pedal);

    postNotice (lighthost::scan::crashedPluginsNotice (added));
}

void IconMenu::saveSuspendedPlugins()
{
    auto* const settings = getAppProperties().getUserSettings();
    settings->setValue ("suspendedPlugins", lighthost::guard::suspendedToString (controller.getSuspendedFiles()));
    settings->saveIfNeeded();
}

// A guard file left from the last run names the plugin that was loading when the process died. Suspend its
// file (persistently: every node using it loads as a stand-in, in the live graph and in every preset),
// blacklist it, save both BEFORE the guard is deleted, and tell the user without a dialog.
void IconMenu::recoverLoadGuard()
{
    auto* const settings = getAppProperties().getUserSettings();
    const auto guardFile = lighthost::scan::perInstanceFile (settings->getFile(), "LoadingPlugin");

    controller.setSuspendedFiles (lighthost::guard::suspendedFromString (settings->getValue ("suspendedPlugins")));
    controller.setLoadGuardFile (guardFile);
    controller.onSuspendedChanged = [this] { saveSuspendedPlugins(); };

    const auto record = lighthost::guard::readGuard (guardFile);
    if (! record.has_value())
    {
        lighthost::guard::clearGuard (guardFile);   // empty or damaged file
        return;
    }

    auto suspended = controller.getSuspendedFiles();
    suspended.addIfNotAlreadyThere (record->path);
    controller.setSuspendedFiles (suspended);
    lighthost::scan::blacklistCrashedPlugins (knownPluginList, StringArray (record->path));

    settings->setValue ("suspendedPlugins", lighthost::guard::suspendedToString (suspended));
    if (auto xml = knownPluginList.createXml())
        settings->setValue ("pluginList", xml.get());
    if (! settings->saveIfNeeded())
        return;   // keep the guard: the next start tries again (the suspension still holds this session)

    lighthost::guard::clearGuard (guardFile);
    postNotice (lighthost::guard::loadGuardNotice (record->name));
}

IconMenu::~IconMenu()
{
    shutdownCleanly();
}

void IconMenu::shutdownCleanly()
{
    if (isShutDown)
        return;
    isShutDown = true;
    midiListConnection.reset();
    hotkeys.reset();   // unregisters every shortcut and destroys its window first
    if (shortcutDialog != nullptr)
        shortcutDialog->exitModalState (0);
    shortcutDialog = nullptr;

    // UI stops polling the controller before anything below is torn down.
    stopTimer();
    mainWindow = nullptr;
    pluginListWindow = nullptr;

    // Close editors BEFORE saving: some plugins update their state when the editor closes.
    PluginWindow::closeAllCurrentlyOpenWindows();

    persistPresets();
    controller.save (*getAppProperties().getUserSettings());

    // Stop the device before the graph and plugin instances are destroyed.
    deviceManager.removeMidiInputDeviceCallback ({}, &player);
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (&player);
    player.setProcessor (nullptr);
    deviceManager.closeAudioDevice();
}

//== Presets ===================================================================
void IconMenu::loadPresets()
{
    auto& settings = *getAppProperties().getUserSettings();
    const String rawPresets = settings.getValue ("presets");
    store = PresetStore::fromXml (rawPresets);

    // An empty value is a normal first run; a non-empty one that doesn't parse would be
    // overwritten by the re-seeded "Default" on the next save, so keep a copy first.
    presetsWarning = {};
    if (rawPresets.isNotEmpty() && ! store.isValid())
    {
        const auto backup = lighthost::backupUnreadable (settings, "presets", rawPresets);
        presetsWarning = "Your saved presets could not be read. "
                       + (backup != File() ? "A copy was kept at " + backup.getFullPathName() + ". "
                                           : String ("It could not be copied either. "))
                       + "The app started with defaults.";
    }

    // Migration / first run: never leave the store empty — seed a "Default" preset
    // from the current live document so an existing chain is preserved as a scene.
    if (! store.isValid() || store.getNumPresets() == 0)
    {
        store = PresetStore();
        store.addPreset ("Default", controller.snapshotDocument());
        store.setActiveIndex (0);
    }
}

void IconMenu::persistPresets()
{
    auto& settings = *getAppProperties().getUserSettings();
    // Fold the live document into the active preset so "last active" and the active
    // preset agree on the next launch.
    if (const int active = store.getActiveIndex(); active >= 0)
        store.setPresetDocument (active, controller.snapshotDocument());
    settings.setValue ("presets", store.toXml());
    settings.saveIfNeeded();
}

void IconMenu::switchToPreset (int index)
{
    if (index < 0 || index >= store.getNumPresets() || index == store.getActiveIndex())
        return;

    if (presetLoadBusy)
        return;   // re-entered from a nested message loop while a document loads
    const BusyScope busy (presetLoadBusy);

    auto& settings = *getAppProperties().getUserSettings();
    if (const int active = store.getActiveIndex(); active >= 0)
        store.setPresetDocument (active, controller.snapshotDocument());   // save current

    store.setActiveIndex (index);
    controller.loadDocument (store.getPresetDocument (index));             // load target

    controller.save (settings);
    settings.setValue ("presets", store.toXml());
    settings.saveIfNeeded();

    if (mainWindow != nullptr)
    {
        mainWindow->refreshChain();
        mainWindow->refreshPresets();
    }
}

void IconMenu::addPreset()
{
    const String name = promptForName ("New Preset", "Preset " + String (store.getNumPresets() + 1));
    if (name.isEmpty() || presetLoadBusy)
        return;
    const BusyScope busy (presetLoadBusy);

    // Snapshot the current scene, then add an EMPTY preset (IN -> OUT) and load it like a preset switch.
    if (const int active = store.getActiveIndex(); active >= 0)
        store.setPresetDocument (active, controller.snapshotDocument());
    const int idx = store.addPreset (name, PresetStore::emptyPresetDocument());
    store.setActiveIndex (idx);
    controller.loadDocument (store.getPresetDocument (idx));   // repairs / pins the OUT like any stored preset

    auto& settings = *getAppProperties().getUserSettings();
    controller.save (settings);
    settings.setValue ("presets", store.toXml());
    settings.saveIfNeeded();

    if (mainWindow != nullptr)
    {
        mainWindow->refreshChain();
        mainWindow->refreshPresets();
    }
}

void IconMenu::duplicatePreset (int index)
{
    const int active = store.getActiveIndex();
    if (index < 0 || index >= store.getNumPresets())
        return;

    // Fold the live graph (plugin states included) into the active preset first, so the copy of the active
    // one is the live scene.
    if (active >= 0)
        store.setPresetDocument (active, controller.snapshotDocument());

    const int copy = store.duplicatePreset (index);
    if (copy < 0)
        return;
    if (index == active)
        store.setActiveIndex (copy);   // live doc already equals the copy: no reload, plugins stay loaded

    auto& settings = *getAppProperties().getUserSettings();
    settings.setValue ("presets", store.toXml());
    settings.saveIfNeeded();

    if (mainWindow != nullptr)
        mainWindow->refreshPresets();
}

void IconMenu::renamePreset (int index)
{
    if (index < 0 || index >= store.getNumPresets())
        return;
    const String name = promptForName ("Rename Preset", store.getPresetName (index));
    if (name.isEmpty())
        return;

    store.setPresetName (index, name);
    auto& settings = *getAppProperties().getUserSettings();
    settings.setValue ("presets", store.toXml());
    settings.saveIfNeeded();

    if (mainWindow != nullptr)
        mainWindow->refreshPresets();
}

void IconMenu::saveActivePreset()
{
    const int active = store.getActiveIndex();
    if (active < 0)
        return;
    store.setPresetDocument (active, controller.snapshotDocument());
    auto& settings = *getAppProperties().getUserSettings();
    settings.setValue ("presets", store.toXml());
    settings.saveIfNeeded();
}

void IconMenu::deletePreset (int index)
{
    if (index < 0 || index >= store.getNumPresets() || presetLoadBusy)
        return;
    const BusyScope busy (presetLoadBusy);

    // Fold the live graph into the active preset before the indices move.
    const int active = store.getActiveIndex();
    if (active >= 0)
        store.setPresetDocument (active, controller.snapshotDocument());

    auto& settings = *getAppProperties().getUserSettings();

    if (index != active)
    {
        // Another tab: the live graph stays as it is; the store keeps pointing at the same active preset.
        store.removePreset (index);
        settings.setValue ("presets", store.toXml());
        settings.saveIfNeeded();
        syncHotkeys();
        if (mainWindow != nullptr)
            mainWindow->refreshPresets();
        return;
    }

    store.removePreset (active);
    if (store.getNumPresets() == 0)   // never leave 0 presets
    {
        store.addPreset ("Default", controller.snapshotDocument());
        store.setActiveIndex (0);
    }

    const int newActive = store.getActiveIndex();
    controller.loadDocument (store.getPresetDocument (newActive));

    controller.save (settings);
    settings.setValue ("presets", store.toXml());
    settings.saveIfNeeded();
    syncHotkeys();

    if (mainWindow != nullptr)
    {
        mainWindow->refreshChain();
        mainWindow->refreshPresets();
    }
}

// Reorders the tabs. The graph stays loaded: only the order (and the active index) changes.
void IconMenu::movePreset (int from, int to)
{
    if (from < 0 || from >= store.getNumPresets() || to < 0 || to >= store.getNumPresets() || from == to)
        return;

    if (const int active = store.getActiveIndex(); active >= 0)
        store.setPresetDocument (active, controller.snapshotDocument());   // fold the live graph in first
    if (! store.movePreset (from, to))
        return;

    auto& settings = *getAppProperties().getUserSettings();
    settings.setValue ("presets", store.toXml());
    settings.saveIfNeeded();

    if (mainWindow != nullptr)
        mainWindow->refreshPresets();
}

//== Global preset shortcuts ===================================================
// Registers the stored combinations (replacing any earlier registration). Nothing is created while no preset has a
// shortcut. A combination another app holds stays bound to its preset and is reported without a dialog.
void IconMenu::syncHotkeys()
{
    if (! shortcutsAllowed || isShutDown)
        return;

    const auto combos = store.registeredShortcuts();
    if (hotkeys == nullptr)
    {
        if (combos.isEmpty())
            return;
        hotkeys = std::make_unique<PresetHotkeys>();
        hotkeys->onPressed = [this] (const String& combo) { handlePresetHotkey (combo); };
    }

    const auto failedNow = hotkeys->sync (combos);
    for (const auto& failed : failedNow)
        if (! reportedHotkeyFailures.contains (failed))   // once per combination, not after every later change
            postNotice ("Shortcut " + failed + " is used by another app");
    reportedHotkeyFailures = failedNow;
}

// A press arrives here through the message queue, never from inside the window procedure. It is dropped while a
// document is loading (a plugin dialog can run a nested message loop that delivers it) and while a modal
// component (rename prompt, shortcut capture) is open.
void IconMenu::handlePresetHotkey (const String& combo)
{
    if (isShutDown || presetLoadBusy || Component::getCurrentlyModalComponent() != nullptr)
        return;

    if (const auto parsed = lighthost::hotkeys::parse (combo); parsed.has_value())
        if (const int index = store.findPresetByShortcut (*parsed); index >= 0)
            switchToPreset (index);
}

void IconMenu::setPresetShortcut (int index)
{
    if (! shortcutsAllowed || shortcutDialog != nullptr || index < 0 || index >= store.getNumPresets())
        return;

    // Our own hotkeys would swallow the very keystroke being captured.
    if (hotkeys != nullptr)
        hotkeys->sync ({});

    shortcutDialog = std::make_unique<lighthost::ui::ShortcutCaptureWindow> (
        store.getPresetName (index), store.getPresetShortcut (index),
        [this, index] (std::optional<lighthost::hotkeys::Combo> result) { finishShortcutCapture (index, std::move (result)); });
}

// Every way out of the dialog (OK, Esc, Cancel, the close button) ends up here and re-registers the hotkeys.
void IconMenu::finishShortcutCapture (int index, std::optional<lighthost::hotkeys::Combo> result)
{
    if (shortcutDialog != nullptr)
        shortcutDialog->exitModalState (0);

    if (result.has_value() && index >= 0 && index < store.getNumPresets())
    {
        const int previous = store.setPresetShortcut (index, *result);
        if (previous >= 0)
            postNotice ("Shortcut " + lighthost::hotkeys::format (*result) + " moved from \"" + store.getPresetName (previous) + "\"");

        auto& settings = *getAppProperties().getUserSettings();
        settings.setValue ("presets", store.toXml());
        settings.saveIfNeeded();
        if (mainWindow != nullptr)
            mainWindow->refreshPresets();
    }

    syncHotkeys();   // always: the capture turned every shortcut off

    // The dialog is inside its own callback: delete it from the queue.
    MessageManager::callAsync ([this]
    {
        if (! isShutDown)
            shortcutDialog = nullptr;
    });
}

void IconMenu::clearPresetShortcut (int index)
{
    if (index < 0 || index >= store.getNumPresets())
        return;

    store.clearPresetShortcut (index);
    auto& settings = *getAppProperties().getUserSettings();
    settings.setValue ("presets", store.toXml());
    settings.saveIfNeeded();
    syncHotkeys();
    if (mainWindow != nullptr)
        mainWindow->refreshPresets();
}

String IconMenu::promptForName (const String& title, const String& initial)
{
    AlertWindow w (title, "Preset name:", MessageBoxIconType::NoIcon);
    w.addTextEditor ("name", initial, {});
    w.addButton ("OK",     1, KeyPress (KeyPress::returnKey));
    w.addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    if (w.runModalLoop() == 1)
        return w.getTextEditorContents ("name").trim();
    return {};
}

void IconMenu::setIcon()
{
    #if JUCE_MAC
    // Template image: macOS recolours it automatically for light/dark menu bars.
    Image icon (ImageFileFormat::loadFrom (BinaryData::menu_icon_png, BinaryData::menu_icon_pngSize));
    setIconImage (icon, icon);
    #else
    String defaultColor;
    #if JUCE_WINDOWS
    defaultColor = "white";
    #elif JUCE_LINUX
    defaultColor = "black";
    #endif
    if (! getAppProperties().getUserSettings()->containsKey ("icon"))
        getAppProperties().getUserSettings()->setValue ("icon", defaultColor);
    String color = getAppProperties().getUserSettings()->getValue ("icon");
    Image icon;
    if (color.equalsIgnoreCase ("white"))
        icon = ImageFileFormat::loadFrom (BinaryData::menu_icon_white_png, BinaryData::menu_icon_white_pngSize);
    else if (color.equalsIgnoreCase ("black"))
        icon = ImageFileFormat::loadFrom (BinaryData::menu_icon_png, BinaryData::menu_icon_pngSize);
    setIconImage (icon, icon);
    #endif
}

lighthost::outputs::Layout IconMenu::currentOutputLayout() const
{
    lighthost::outputs::Layout layout;
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        layout.known      = true;
        layout.deviceName = device->getName();
        layout.names      = device->getOutputChannelNames();
        layout.activeMask = device->getActiveOutputChannels();
    }
    return layout;
}

void IconMenu::applyDeviceLayout()
{
    if (isShutDown)
        return;
    // The device manager also reports unrelated changes (MIDI inputs...): rebuild the cards only when the
    // layout or a binding really changed.
    bool changed = controller.setOutputLayout (currentOutputLayout());   // one rewire, only if some OUT's pair moved
    changed = pinLegacyOutputs (false) || changed;
    if (changed && mainWindow != nullptr)
        mainWindow->refreshChain();   // OUT cards show active / inactive
}

bool IconMenu::pinLegacyOutputs (bool alsoSave)
{
    bool changed = alsoSave;
    if (const auto target = lighthost::outputs::pinTarget (controller.getOutputLayout()))
    {
        changed = controller.pinLegacyOutputs (*target) || changed;
        changed = store.pinLegacyOutputs (target->outCh, target->label, target->device) || changed;
    }
    if (changed)
    {
        persistPresets();
        controller.save (*getAppProperties().getUserSettings());
    }
    return changed;
}

void IconMenu::changeListenerCallback (ChangeBroadcaster* changed)
{
    if (changed == &deviceManager)
        return applyDeviceLayout();

    if (changed == &knownPluginList)
    {
        auto savedPluginList = knownPluginList.createXml();
        if (savedPluginList != nullptr)
        {
            getAppProperties().getUserSettings()->setValue ("pluginList", savedPluginList.get());
            getAppProperties().saveIfNeeded();
        }
    }
}

void IconMenu::timerCallback()
{
    stopTimer();
    menu.clear();
    menu.addSectionHeader (JUCEApplication::getInstance()->getApplicationName());
    if (menuIconLeftClicked)
    {
        menu.addItem (1, "Preferences");
        menu.addItem (2, "Edit Plugins");
        menu.addItem (kShowWindowMenuId, "Show Editor Window");
        menu.addItem (kMonoInputMenuId, "Mono Input", true, controller.isMonoInput());
        #if JUCE_WINDOWS
        // Not offered to -multi-instance copies: they would fight over the single Run value.
        if (! runsAsMultiInstance())
        {
            const auto autostart = lighthost::autostart::read();   // read only when the menu is built
            menu.addItem (kStartWithWindowsMenuId,
                          autostart == lighthost::autostart::State::otherExe ? "Start with Windows (other copy)" : "Start with Windows",
                          true, autostart != lighthost::autostart::State::off);
        }
        #endif
        // Presets switcher — change the active scene from the background.
        {
            PopupMenu presets;
            for (int i = 0; i < store.getNumPresets(); ++i)
                presets.addItem (INDEX_PRESET + i, store.getPresetName (i), true, i == store.getActiveIndex());
            menu.addSubMenu ("Presets", presets);
        }
        menu.addSeparator();
        menu.addSectionHeader ("Active Plugins");
        // Missing plugins stay pass-through until retried explicitly (finding 6).
        if (const int numMissing = controller.getNumMissingPlugins(); numMissing > 0)
            menu.addItem (kRetryMissingMenuId, "Retry missing plugins (" + String (numMissing) + ")");
        // Move Up/Down rewrite the graph as a straight chain, which would clobber
        // parallel routing made on the canvas — so only they are gated on a linear chain.
        // Add and Delete are safe on a branched graph.
        const bool linear = controller.isLinearChain();
        if (! linear)
            menu.addItem (4, "Move Up/Down disabled: graph has parallel routing - edit on the canvas.", false);
        // Active plugins. Every per-plugin item carries its uid in an action lambda, so
        // an item always acts on the plugin its label named, even if a second tray click
        // rebuilt the menu or the graph changed while this one was open. Uids are
        // re-validated at fire time: a stale click is a no-op.
        const auto chain = controller.getChain();
        for (int i = 0; i < (int) chain.size(); i++)
        {
            const auto& item = chain[(size_t) i];
            const String uid = item.uid;

            // Runs `edit` on the plugin if it still exists, then persists, refreshes the
            // open editor window and re-arms the tray menu.
            const auto onPlugin = [this, uid] (std::function<void()> edit)
            {
                return [this, uid, edit = std::move (edit)]
                {
                    if (! controller.document.getNodeByUid (uid).isValid())
                        return;
                    edit();
                    controller.save (*getAppProperties().getUserSettings());
                    if (mainWindow != nullptr)
                        mainWindow->refreshChain();
                    startTimer (50);
                };
            };

            PopupMenu options;
            options.addItem ("Edit", ! item.missing, false, onPlugin ([this, uid]
            {
                if (auto* const node = controller.getNodeForUid (uid))
                    if (auto* const w = PluginWindow::getWindowFor (node, PluginWindow::Normal))
                        w->toFront (true);
            }));
            options.addItem ("Bypass", true, item.bypassed, onPlugin ([this, uid]
            {
                // Toggles the CURRENT doc state, not the state at menu-build time.
                controller.setBypassed (uid, ! controller.document.isBypassed (uid));
            }));
            options.addSeparator();
            // Serial moves are re-checked at fire time: the menu may predate a canvas
            // edit that branched the graph.
            options.addItem ("Move Up", linear && i > 0, false, onPlugin ([this, uid]
            {
                if (controller.isLinearChain())
                    controller.moveUp (uid);
            }));
            options.addItem ("Move Down", linear && i < (int) chain.size() - 1, false, onPlugin ([this, uid]
            {
                if (controller.isLinearChain())
                    controller.moveDown (uid);
            }));
            options.addSeparator();
            // Delete heals around the node (removeNode), so unlike the serial
            // move/add ops it is safe on a branched graph and stays enabled.
            options.addItem ("Delete", true, false, onPlugin ([this, uid] { controller.removeNode (uid); }));
            menu.addSubMenu (item.missing ? item.name + " (missing)" : item.name, options);
        }
        menu.addSeparator();
        menu.addSectionHeader ("Available Plugins");
        // All plugins. addPlugin appends serially on a linear chain and adds unconnected
        // on a branched graph, so adding is never gated.
        KnownPluginList::addToMenu (menu, loadablePlugins(), pluginSortMethod);
    }
    else
    {
        menu.addItem (1, "Quit");
        menu.addSeparator();
        menu.addItem (2, "Delete Plugin States");
        #if ! JUCE_MAC
        menu.addItem (3, "Invert Icon Color");
        #endif
    }
    #if JUCE_MAC || JUCE_LINUX
    menu.showMenuAsync (PopupMenu::Options().withTargetComponent (this), ModalCallbackFunction::forComponent (menuInvocationCallback, this));
    #else
    if (x == 0 || y == 0)
    {
        POINT iconLocation;
        iconLocation.x = 0;
        iconLocation.y = 0;
        GetCursorPos (&iconLocation);
        x = iconLocation.x;
        y = iconLocation.y;
    }
    juce::Rectangle<int> rect (x, y, 1, 1);
    menu.showMenuAsync (PopupMenu::Options().withTargetScreenArea (rect), ModalCallbackFunction::forComponent (menuInvocationCallback, this));
    #endif
}

void IconMenu::mouseDown (const MouseEvent& e)
{
    #if JUCE_MAC
    Process::setDockIconVisible (true);
    #endif
    Process::makeForegroundProcess();
    menuIconLeftClicked = e.mods.isLeftButtonDown();
    #if JUCE_WINDOWS
    // A left click may be the first half of a double click (which opens the window), so the menu waits out the
    // system double-click time; mouseDoubleClick cancels it. A right click shows the menu at once.
    startTimer (menuIconLeftClicked ? jmax (50, MouseEvent::getDoubleClickTimeout()) : 50);
    #else
    startTimer (50);
    #endif
}

void IconMenu::mouseDoubleClick (const MouseEvent& e)
{
    if (! e.mods.isLeftButtonDown())
        return;
    stopTimer();   // drop the menu the first click scheduled
    showMainWindow();   // opens it if hidden, raises it if open
}

void IconMenu::menuInvocationCallback (int id, IconMenu* im)
{
    auto& settings = *getAppProperties().getUserSettings();

    // Right click
    if (! im->menuIconLeftClicked)
    {
        if (id == 1)
        {
            im->persistPresets();
            im->controller.save (settings);
            return JUCEApplication::getInstance()->quit();
        }
        if (id == 2)
        {
            im->controller.clearAllPluginStates();
            im->controller.save (settings);
            // The rebuild may have replaced instances (or turned an uninstalled
            // plugin into a pass-through stand-in) — rebuild the cards so they
            // never keep polling params of a stale instance.
            if (im->mainWindow != nullptr)
                im->mainWindow->refreshChain();
            return;
        }
        if (id == 3)
        {
            String color = settings.getValue ("icon");
            settings.setValue ("icon", color.equalsIgnoreCase ("black") ? "white" : "black");
            return im->setIcon();
        }
    }
    #if JUCE_MAC
    // Click elsewhere
    if (id == 0 && ! PluginWindow::containsActiveWindows())
        Process::setDockIconVisible (false);
    #endif
    // Audio settings
    if (id == 1)
        im->showAudioSettings();
    // Reload
    if (id == 2)
        im->reloadPlugins();
    // Show the main editor window
    if (id == kShowWindowMenuId)
        return im->showMainWindow();
    // Mono Input toggle (sum input to both channels). Persist + refresh the tick.
    if (id == kMonoInputMenuId)
    {
        const bool on = ! im->controller.isMonoInput();
        im->controller.setMonoInput (on);
        settings.setValue ("monoInput", on);
        settings.saveIfNeeded();
        return im->startTimer (50);
    }
    #if JUCE_WINDOWS
    // Off or pointing at another copy -> point it at this exe; pointing at this exe -> remove it.
    if (id == kStartWithWindowsMenuId)
    {
        const bool remove = lighthost::autostart::read() == lighthost::autostart::State::thisExe;
        if (! (remove ? lighthost::autostart::disable() : lighthost::autostart::enableForThisExe()))
            im->postNotice ("Could not change the Start with Windows setting.");
        return;
    }
    #endif
    // Retry plugins that failed to load. The document is unchanged (their saved
    // state was kept), so there is nothing to save; just rebuild the cards.
    if (id == kRetryMissingMenuId)
    {
        if (im->controller.retryMissingPlugins() > 0 && im->mainWindow != nullptr)
            im->mainWindow->refreshChain();
        return;
    }
    // Preset switch (from the tray Presets submenu). Handled first so it never
    // falls through to the serial chain-op decoding below.
    if (id >= im->INDEX_PRESET && id < im->INDEX_PRESET + 1000000)
        return im->switchToPreset (id - im->INDEX_PRESET);
    // Add plugin (appends while linear, adds unconnected on a branched graph). The
    // per-plugin items (Edit/Bypass/Move/Delete) are action lambdas, not ids.
    const auto loadable = im->loadablePlugins();
    if (id > 2 && KnownPluginList::getIndexChosenByMenu (loadable, id) > -1)
    {
        const auto knownTypes = loadable;
        const PluginDescription plugin = knownTypes[KnownPluginList::getIndexChosenByMenu (knownTypes, id)];
        const String uid = im->controller.addPlugin (plugin);
        im->controller.save (settings);
        // Reflect the chain change in the open editor window, then update the menu.
        if (im->mainWindow != nullptr)
        {
            im->mainWindow->refreshChain();
            im->mainWindow->revealNode (uid);
        }
        im->startTimer (50);
    }
}

void IconMenu::showAudioSettings()
{
    AudioDeviceSelectorComponent audioSettingsComp (deviceManager, 0, 256, 0, 256, false, false, true, true);
    audioSettingsComp.setSize (500, 450);

    DialogWindow::LaunchOptions o;
    o.content.setNonOwned (&audioSettingsComp);
    o.dialogTitle                   = "Audio Settings";
    o.componentToCentreAround       = this;
    o.dialogBackgroundColour        = Colour::fromRGB (236, 236, 236);
    o.escapeKeyTriggersCloseButton  = true;
    o.useNativeTitleBar             = true;
    o.resizable                     = false;

    o.runModal();

    auto audioState = deviceManager.createStateXml();

    getAppProperties().getUserSettings()->setValue ("audioDeviceState", audioState.get());
    getAppProperties().getUserSettings()->saveIfNeeded();
}

void IconMenu::reloadPlugins()
{
    if (pluginListWindow == nullptr)
        pluginListWindow = std::make_unique<PluginListWindow> (*this, formatManager);
    pluginListWindow->toFront (true);
}

void IconMenu::showMainWindow()
{
    if (mainWindow == nullptr)
    {
        lighthost::ui::MainComponent::Callbacks cb;
        cb.addPlugin    = [this] (juce::Point<int> p) { showAddPluginMenu (p); };
        cb.openEditor   = [this] (const String& uid)  { openEditorForUid (uid); };
        cb.replacePlugin = [this] (const String& uid, juce::Point<int> p) { showReplacePluginMenu (uid, p); };
        cb.preferences  = [this] { showAudioSettings(); };
        cb.editPlugins  = [this] { reloadPlugins(); };
        cb.selectPreset = [this] (int i) { switchToPreset (i); };
        cb.addPreset    = [this] { addPreset(); };
        cb.renamePreset = [this] (int i) { renamePreset (i); };
        cb.duplicatePreset = [this] (int i) { duplicatePreset (i); };
        cb.savePreset   = [this] { saveActivePreset(); };
        cb.deletePreset = [this] (int i) { deletePreset (i); };
        cb.movePreset   = [this] (int from, int to) { movePreset (from, to); };
        cb.shortcutsAvailable = [this] { return shortcutsAllowed; };
        cb.setShortcut  = [this] (int i) { setPresetShortcut (i); };
        cb.clearShortcut = [this] (int i) { clearPresetShortcut (i); };
        cb.sampleRate   = [this]
        {
            auto* d = deviceManager.getCurrentAudioDevice();
            return d != nullptr ? d->getCurrentSampleRate() : 0.0;
        };
        cb.cpuLoad      = [this] { return deviceManager.getCpuUsage(); };
        mainWindow = std::make_unique<lighthost::ui::MainWindow> (controller, store, std::move (cb));
    }

    #if JUCE_MAC
    Process::setDockIconVisible (true);
    #endif
    mainWindow->setVisible (true);
    mainWindow->toFront (true);

    if (startupNotice.isNotEmpty())
    {
        mainWindow->showNotice (startupNotice);
        startupNotice = {};
    }
}

void IconMenu::showAddPluginMenu (juce::Point<int> screenPos)
{
    // Effects keep the manufacturer-sorted list; instruments (sources, no audio input) get their own
    // submenu with plain ids 1..N (the list ids start far above, so they can't collide).
    Array<PluginDescription> effects, instruments;
    for (const auto& t : loadablePlugins())
        (GraphDocument::isSourceDescription (t) ? instruments : effects).add (t);

    PopupMenu m;
    KnownPluginList::addToMenu (m, effects, pluginSortMethod);
    if (! instruments.isEmpty())
    {
        PopupMenu synths;
        for (int i = 0; i < instruments.size(); ++i)
            synths.addItem (i + 1, instruments.getReference (i).name);
        m.addSeparator();
        m.addSubMenu ("Instruments", synths);
    }

    // Output > the open device's channel pairs (inactive ones marked). Ids are far above the instrument ids.
    constexpr int outputIdBase = 1000000;
    const auto layout = controller.getOutputLayout();
    const int numPairs = layout.known ? layout.names.size() / 2 : 0;
    if (layout.known && layout.names.size() >= 2)
    {
        PopupMenu outs;
        for (int p = 0; p < layout.names.size() / 2; ++p)
        {
            const bool active = lighthost::outputs::activeIndex (layout.activeMask, 2 * p) >= 0;
            outs.addItem (outputIdBase + p, lighthost::outputs::pairLabel (layout.names, 2 * p) + (active ? "" : "  (inactive)"));
        }
        m.addSeparator();
        m.addSubMenu ("Output", outs);
    }

    m.showMenuAsync (PopupMenu::Options()
                        .withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
        [this, effects, instruments, outputIdBase, numPairs] (int r)
        {
            if (r <= 0)
                return;
            const auto choice = lighthost::choice::decodeAddMenuId (r, instruments.size(), outputIdBase, numPairs, effects);
            if (choice.kind == lighthost::choice::AddMenuChoice::Kind::none)
                return;
            if (choice.kind == lighthost::choice::AddMenuChoice::Kind::output)
            {
                const String uid = controller.addOutput (2 * choice.index);
                if (uid.isEmpty())
                    return;
                controller.save (*getAppProperties().getUserSettings());
                if (mainWindow != nullptr)
                {
                    mainWindow->refreshChain();
                    mainWindow->revealNode (uid);
                }
                return;
            }
            const PluginDescription* picked = choice.kind == lighthost::choice::AddMenuChoice::Kind::instrument
                                                  ? &instruments.getReference (choice.index)
                                                  : &effects.getReference (choice.index);
            const String uid = controller.addPlugin (*picked);
            controller.save (*getAppProperties().getUserSettings());
            if (mainWindow != nullptr)
            {
                mainWindow->refreshChain();
                mainWindow->revealNode (uid);   // the floating ADD may be far from the new node
            }
        });
}

juce::Array<PluginDescription> IconMenu::loadablePlugins() const
{
    StringArray formats;
    for (int i = 0; i < formatManager.getNumFormats(); ++i)
        formats.add (formatManager.getFormat (i)->getName());
    StringArray hidden (controller.getSuspendedFiles());
    hidden.addArray (knownPluginList.getBlacklistedFiles());
    return lighthost::choice::filterLoadable (knownPluginList.getTypes(), formats, hidden);
}

// Card menu -> Replace with…: loadable plugins only, the likely successors on top. Non-modal
// (async popup), so it never re-enters the message loop in the middle of an edit.
void IconMenu::showReplacePluginMenu (const String& uid, juce::Point<int> screenPos)
{
    const auto oldDesc = GraphDocument::getPluginDescription (controller.document.getNodeByUid (uid));
    if (! oldDesc.has_value())
        return;

    const auto types = loadablePlugins();
    const auto suggested = lighthost::choice::suggestReplacements (*oldDesc, types);

    PopupMenu m;
    if (! suggested.empty())
    {
        m.addSectionHeader ("Suggested");
        for (size_t i = 0; i < suggested.size(); ++i)
            m.addItem ((int) i + 1, types.getReference (suggested[i]).name + "  (" + types.getReference (suggested[i]).pluginFormatName + ")");
        m.addSeparator();
        m.addSectionHeader ("All plugins");
    }
    KnownPluginList::addToMenu (m, types, pluginSortMethod);

    m.showMenuAsync (PopupMenu::Options().withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
        [this, uid, types, suggested, oldDesc = *oldDesc] (int r)
        {
            if (r <= 0)
                return;

            int idx = -1;
            if (r <= (int) suggested.size())
                idx = suggested[(size_t) r - 1];
            else
                idx = KnownPluginList::getIndexChosenByMenu (types, r);
            if (idx < 0)
                return;

            // A node re-added from a stale VST2 entry has no state; the original may still sit in the legacy key.
            auto& settings = *getAppProperties().getUserSettings();
            MemoryBlock legacy;
            legacy.fromBase64Encoding (settings.getValue (GraphDocument::legacyKey ("state", oldDesc)));

            const auto result = controller.replacePlugin (uid, types[idx], legacy.isEmpty() ? nullptr : &legacy);
            if (result.ok)
                controller.save (settings);

            // A by-position import goes through setValueNotifyingHost, which a split-component VST3 hands to
            // its processor only in the next process block; the save above can still see defaults. One
            // delayed save (no permanent timer) picks the values up; repeated replaces share it.
            if (result.ok && result.import == GraphController::ReplaceResult::Import::paramsByIndex && ! deferredSavePending)
            {
                deferredSavePending = true;
                auto* self = this;   // MSVC rejects `this` inside the nested init-capture
                Timer::callAfterDelay (1000, [safe = Component::SafePointer<IconMenu> (self)]
                {
                    if (safe == nullptr)
                        return;
                    safe->deferredSavePending = false;
                    if (! safe->isShutDown)
                        safe->controller.save (*getAppProperties().getUserSettings());
                });
            }
            if (mainWindow != nullptr)
            {
                if (result.ok)
                    mainWindow->refreshChain();
                mainWindow->showNotice (result.message);
            }
        });
}

void IconMenu::openEditorForUid (const String& uid)
{
    if (auto* const node = controller.getNodeForUid (uid))
        if (auto* const w = PluginWindow::getWindowFor (node, PluginWindow::Normal))
            w->toFront (true);
}

void IconMenu::removePluginsLackingInputOutput()
{
    // NOTE: channel counts in PluginDescription are unreliable for VST3;
    // this preserves upstream behaviour and will be revisited with the GUI.
    // Instruments (no audio input) are kept as sources: the isInstrument flag alone is enough, because
    // VST3 channel counts in descriptions are unreliable. Effects keep the stereo in/out rule.
    for (const auto& plugin : knownPluginList.getTypes())
    {
        const bool keep = GraphDocument::isSourceDescription (plugin)
                              ? plugin.numOutputChannels >= 1 || plugin.isInstrument
                              : plugin.numInputChannels >= 2 && plugin.numOutputChannels >= 2;
        if (! keep)
            knownPluginList.removeType (plugin);
    }
}
