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

namespace lighthost::ui { class MainWindow; }

ApplicationProperties& getAppProperties();

class IconMenu : public SystemTrayIconComponent, private Timer, public ChangeListener
{
public:
    IconMenu();
    ~IconMenu() override;
    void mouseDown (const MouseEvent&) override;
    static void menuInvocationCallback (int id, IconMenu*);
    void changeListenerCallback (ChangeBroadcaster* changed) override;

    const int INDEX_EDIT, INDEX_BYPASS, INDEX_DELETE, INDEX_MOVE_UP, INDEX_MOVE_DOWN, INDEX_PRESET;

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
    void persistPresets();                 // snapshot active + write settings
    void switchToPreset (int index);
    void addPreset();
    void renamePreset (int index);
    void saveActivePreset();
    void deleteActivePreset();
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
    #if JUCE_WINDOWS
    int x, y;
    #endif

    class PluginListWindow;
    std::unique_ptr<PluginListWindow> pluginListWindow;
    std::unique_ptr<lighthost::ui::MainWindow> mainWindow;
};

#endif /* IconMenu_hpp */
