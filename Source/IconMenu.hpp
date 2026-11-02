//
//  IconMenu.hpp
//  Light Host
//
//  Created by Rolando Islas on 12/26/15.
//  Milestone 2 (2026): chain state lives in GraphDocument/GraphController.
//

#ifndef IconMenu_hpp
#define IconMenu_hpp

#include <JuceHeader.h>
#include "Engine/GraphController.h"
#include "Engine/PresetStore.h"
#include "PresetHotkeys.h"

namespace lighthost::ui { class MainWindow; class ShortcutCaptureWindow; }

ApplicationProperties& getAppProperties();

class IconMenu : public SystemTrayIconComponent, private Timer, public ChangeListener
{
public:
    explicit IconMenu (juce::String startupWarning = {}, bool safeMode = false);
    ~IconMenu() override;
    void mouseDown (const MouseEvent&) override;
    void mouseDoubleClick (const MouseEvent&) override;
    static void menuInvocationCallback (int id, IconMenu*);
    void changeListenerCallback (ChangeBroadcaster* changed) override;

    const int INDEX_PRESET;

    // Idempotent. Closes editors, saves, then stops the audio device so the graph and
    // plugin instances are never destroyed while the device still calls into them.
    void shutdownCleanly();

private:
    void timerCallback() override;
    void reloadPlugins();
    void showAudioSettings();
    void showMainWindow();
    void showAddPluginMenu (juce::Point<int> screenPos);
    void openEditorForUid (const String& uid);
    void removePluginsLackingInputOutput();
    void setIcon();

    // Presets (scenes). The store is persisted alongside settings under "presets";
    // the active preset mirrors the live controller document.
    void loadPresets();
    // Dead man's pedal of the plugin scan (next to the settings file) and what a crash left in it.
    static File crashedPluginsPedal();
    void recoverCrashedPlugins();
    // Load guard: a plugin that killed the last start is blacklisted and suspended (stand-in everywhere, also
    // after a restart) until an explicit retry loads it. Runs before the graph is loaded.
    void recoverLoadGuard();
    void saveSuspendedPlugins();
    void postNotice (const String& notice);   // main window now, or at its first opening
    void syncMidiInputs();                    // start and every change of the MIDI device list
    // Output pairs (v1.2 B): the open device's active output channels go to the controller, and the
    // unbound OUT nodes (live document + every stored preset) are bound to its first active pair, so
    // enabling more channels later can't move the feed.
    lighthost::outputs::Layout currentOutputLayout() const;
    void applyDeviceLayout();                 // device opened / changed
    bool pinLegacyOutputs (bool alsoSave);    // one settings save when something was stamped; true if it did
    void persistPresets();                 // snapshot active + write settings
    void switchToPreset (int index);
    void addPreset();
    void duplicatePreset (int index);
    void renamePreset (int index);
    void saveActivePreset();
    void deletePreset (int index);
    void movePreset (int from, int to);
    // Global preset shortcuts (Windows; off in -multi-instance and safe mode). The registration object exists only
    // while a shortcut is bound.
    void syncHotkeys();                     // registers the stored combinations, notices for the ones another app holds
    void handlePresetHotkey (const String& combo);
    void setPresetShortcut (int index);     // capture dialog
    void finishShortcutCapture (int index, std::optional<lighthost::hotkeys::Combo> result);
    void clearPresetShortcut (int index);
    String promptForName (const String& title, const String& initial);

    AudioDeviceManager deviceManager;
    AudioPluginFormatManager formatManager;
    KnownPluginList knownPluginList;
    KnownPluginList::SortMethod pluginSortMethod;
    PopupMenu menu;
    bool menuIconLeftClicked;
    AudioProcessorGraph graph;
    AudioProcessorPlayer player;
    GraphController controller { graph, formatManager };
    PresetStore store;
    bool shortcutsAllowed = false;
    bool presetLoadBusy = false;        // a preset document is being loaded; a hotkey press meanwhile is ignored
    std::unique_ptr<PresetHotkeys> hotkeys;
    std::unique_ptr<lighthost::ui::ShortcutCaptureWindow> shortcutDialog;
    bool isShutDown = false;
    bool deferredSavePending = false;   // one delayed save after a by-position import (see showReplacePluginMenu)
    MidiDeviceListConnection midiListConnection;
    StringArray midiSeenIds;   // identifiers of the MIDI inputs listed the last time
    String startupNotice;    // one-line notice for the main window when it first opens
    String presetsWarning;   // set by loadPresets() when the saved presets were unreadable
    #if JUCE_WINDOWS
    int x, y;
    #endif

    // Known plugins this build can actually load (the stored list also holds old VST2 scan entries).
    juce::Array<PluginDescription> loadablePlugins() const;
    void showReplacePluginMenu (const String& uid, juce::Point<int> screenPos);

    class PluginListWindow;
    std::unique_ptr<PluginListWindow> pluginListWindow;
    std::unique_ptr<lighthost::ui::MainWindow> mainWindow;
};

#endif /* IconMenu_hpp */
