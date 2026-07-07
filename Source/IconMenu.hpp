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

ApplicationProperties& getAppProperties();

class IconMenu : public SystemTrayIconComponent, private Timer, public ChangeListener
{
public:
    IconMenu();
    ~IconMenu() override;
    void mouseDown (const MouseEvent&) override;
    static void menuInvocationCallback (int id, IconMenu*);
    void changeListenerCallback (ChangeBroadcaster* changed) override;

    const int INDEX_EDIT, INDEX_BYPASS, INDEX_DELETE, INDEX_MOVE_UP, INDEX_MOVE_DOWN;

private:
    void timerCallback() override;
    void reloadPlugins();
    void showAudioSettings();
    void removePluginsLackingInputOutput();
    void setIcon();

    AudioDeviceManager deviceManager;
    AudioPluginFormatManager formatManager;
    KnownPluginList knownPluginList;
    KnownPluginList::SortMethod pluginSortMethod;
    PopupMenu menu;
    bool menuIconLeftClicked;
    AudioProcessorGraph graph;
    AudioProcessorPlayer player;
    GraphController controller { graph, formatManager };
    #if JUCE_WINDOWS
    int x, y;
    #endif

    class PluginListWindow;
    std::unique_ptr<PluginListWindow> pluginListWindow;
};

#endif /* IconMenu_hpp */
