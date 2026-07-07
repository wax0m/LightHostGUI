//
//  IconMenu.hpp
//  Light Host
//
//  Created by Rolando Islas on 12/26/15.
//
//

#ifndef IconMenu_hpp
#define IconMenu_hpp

#include <JuceHeader.h>
#include <vector>

ApplicationProperties& getAppProperties();

class IconMenu : public SystemTrayIconComponent, private Timer, public ChangeListener
{
public:
    IconMenu();
    ~IconMenu() override;
    void mouseDown (const MouseEvent&) override;
    static void menuInvocationCallback (int id, IconMenu*);
    void changeListenerCallback (ChangeBroadcaster* changed) override;
    static String getKey (String type, PluginDescription plugin);

    const int INDEX_EDIT, INDEX_BYPASS, INDEX_DELETE, INDEX_MOVE_UP, INDEX_MOVE_DOWN;

private:
    void timerCallback() override;
    void reloadPlugins();
    void showAudioSettings();
    void loadActivePlugins();
    void savePluginStates();
    void deletePluginStates();
    PluginDescription getNextPluginOlderThanTime (int& time);
    void removePluginsLackingInputOutput();
    std::vector<PluginDescription> getTimeSortedList();
    void setIcon();

    AudioDeviceManager deviceManager;
    AudioPluginFormatManager formatManager;
    KnownPluginList knownPluginList;
    KnownPluginList activePluginList;
    KnownPluginList::SortMethod pluginSortMethod;
    PopupMenu menu;
    bool menuIconLeftClicked;
    AudioProcessorGraph graph;
    AudioProcessorPlayer player;
    AudioProcessorGraph::Node::Ptr inputNode;
    AudioProcessorGraph::Node::Ptr outputNode;
    #if JUCE_WINDOWS
    int x, y;
    #endif

    class PluginListWindow;
    std::unique_ptr<PluginListWindow> pluginListWindow;
};

#endif /* IconMenu_hpp */
