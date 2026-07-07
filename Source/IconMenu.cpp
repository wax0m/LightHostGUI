//
//  IconMenu.cpp
//  Light Host
//
//  Created by Rolando Islas on 12/26/15.
//  Ported to JUCE 8, 2026.
//

#include <JuceHeader.h>
#include "IconMenu.hpp"
#include "PluginWindow.h"
#include <ctime>
#include <climits>
#if JUCE_WINDOWS
#include "Windows.h"
#endif

using NodeID = AudioProcessorGraph::NodeID;

class IconMenu::PluginListWindow : public DocumentWindow
{
public:
    PluginListWindow (IconMenu& owner_, AudioPluginFormatManager& pluginFormatManager)
        : DocumentWindow ("Available Plugins", Colours::white,
            DocumentWindow::minimiseButton | DocumentWindow::closeButton),
        owner (owner_)
    {
        const File deadMansPedalFile (getAppProperties().getUserSettings()
            ->getFile().getSiblingFile ("RecentlyCrashedPluginsList"));

        setContentOwned (new PluginListComponent (pluginFormatManager,
            owner.knownPluginList,
            deadMansPedalFile,
            getAppProperties().getUserSettings()), true);

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

IconMenu::IconMenu() : INDEX_EDIT (1000000), INDEX_BYPASS (2000000), INDEX_DELETE (3000000), INDEX_MOVE_UP (4000000), INDEX_MOVE_DOWN (5000000)
{
    // Initialization
    addDefaultFormatsToManager (formatManager);   // JUCE 8: replaces AudioPluginFormatManager::addDefaultFormats()
    #if JUCE_WINDOWS
    x = y = 0;
    #endif
    // Audio device
    auto savedAudioState = getAppProperties().getUserSettings()->getXmlValue ("audioDeviceState");
    deviceManager.initialise (256, 256, savedAudioState.get(), true);
    player.setProcessor (&graph);
    deviceManager.addAudioCallback (&player);
    // Plugins - all
    auto savedPluginList = getAppProperties().getUserSettings()->getXmlValue ("pluginList");
    if (savedPluginList != nullptr)
        knownPluginList.recreateFromXml (*savedPluginList);
    pluginSortMethod = KnownPluginList::sortByManufacturer;
    knownPluginList.addChangeListener (this);
    // Plugins - active
    auto savedPluginListActive = getAppProperties().getUserSettings()->getXmlValue ("pluginListActive");
    if (savedPluginListActive != nullptr)
        activePluginList.recreateFromXml (*savedPluginListActive);
    loadActivePlugins();
    activePluginList.addChangeListener (this);
    setIcon();
    setIconTooltip (JUCEApplication::getInstance()->getApplicationName());
}

IconMenu::~IconMenu()
{
    savePluginStates();
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

void IconMenu::loadActivePlugins()
{
    const NodeID INPUT (1000000);
    const NodeID OUTPUT (1000001);
    const int CHANNEL_ONE = 0;
    const int CHANNEL_TWO = 1;
    PluginWindow::closeAllCurrentlyOpenWindows();
    graph.clear();
    inputNode = graph.addNode (std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor> (AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode), INPUT);
    outputNode = graph.addNode (std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor> (AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode), OUTPUT);
    if (activePluginList.getNumTypes() == 0)
    {
        graph.addConnection ({ { INPUT, CHANNEL_ONE }, { OUTPUT, CHANNEL_ONE } });
        graph.addConnection ({ { INPUT, CHANNEL_TWO }, { OUTPUT, CHANNEL_TWO } });
    }
    int pluginTime = 0;
    NodeID lastId;
    bool hasInputConnected = false;
    // NOTE: node ids must not start at 0.
    for (int i = 1; i <= activePluginList.getNumTypes(); i++)
    {
        PluginDescription plugin = getNextPluginOlderThanTime (pluginTime);
        String errorMessage;
        auto instance = formatManager.createPluginInstance (plugin, graph.getSampleRate(), graph.getBlockSize(), errorMessage);
        if (instance == nullptr)
        {
            // Plugin failed to load (missing/blacklisted/broken) — skip it instead of crashing.
            continue;
        }
        String pluginUid = getKey ("state", plugin);
        String savedPluginState = getAppProperties().getUserSettings()->getValue (pluginUid);
        MemoryBlock savedPluginBinary;
        if (savedPluginBinary.fromBase64Encoding (savedPluginState) && savedPluginBinary.getSize() > 0)
            instance->setStateInformation (savedPluginBinary.getData(), (int) savedPluginBinary.getSize());
        const NodeID nodeId ((uint32) i);
        graph.addNode (std::move (instance), nodeId);
        String key = getKey ("bypass", plugin);
        bool bypass = getAppProperties().getUserSettings()->getBoolValue (key, false);
        // Input to plugin
        if (! hasInputConnected && ! bypass)
        {
            graph.addConnection ({ { INPUT, CHANNEL_ONE }, { nodeId, CHANNEL_ONE } });
            graph.addConnection ({ { INPUT, CHANNEL_TWO }, { nodeId, CHANNEL_TWO } });
            hasInputConnected = true;
            lastId = nodeId;
        }
        // Connect previous plugin to current
        else if (! bypass)
        {
            graph.addConnection ({ { lastId, CHANNEL_ONE }, { nodeId, CHANNEL_ONE } });
            graph.addConnection ({ { lastId, CHANNEL_TWO }, { nodeId, CHANNEL_TWO } });
            lastId = nodeId;
        }
    }
    if (lastId.uid > 0)
    {
        // Last active plugin to output
        graph.addConnection ({ { lastId, CHANNEL_ONE }, { OUTPUT, CHANNEL_ONE } });
        graph.addConnection ({ { lastId, CHANNEL_TWO }, { OUTPUT, CHANNEL_TWO } });
    }
}

PluginDescription IconMenu::getNextPluginOlderThanTime (int& time)
{
    int timeStatic = time;
    PluginDescription closest;
    int diff = INT_MAX;
    for (const auto& plugin : activePluginList.getTypes())
    {
        String key = getKey ("order", plugin);
        int pluginTime = getAppProperties().getUserSettings()->getValue (key).getIntValue();
        if (pluginTime > timeStatic && std::abs (timeStatic - pluginTime) < diff)
        {
            diff = std::abs (timeStatic - pluginTime);
            closest = plugin;
            time = pluginTime;
        }
    }
    return closest;
}

void IconMenu::changeListenerCallback (ChangeBroadcaster* changed)
{
    if (changed == &knownPluginList)
    {
        auto savedPluginList = knownPluginList.createXml();
        if (savedPluginList != nullptr)
        {
            getAppProperties().getUserSettings()->setValue ("pluginList", savedPluginList.get());
            getAppProperties().saveIfNeeded();
        }
    }
    else if (changed == &activePluginList)
    {
        auto savedPluginList = activePluginList.createXml();
        if (savedPluginList != nullptr)
        {
            getAppProperties().getUserSettings()->setValue ("pluginListActive", savedPluginList.get());
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
        menu.addSeparator();
        menu.addSectionHeader ("Active Plugins");
        // Active plugins
        const std::vector<PluginDescription> timeSorted = getTimeSortedList();
        for (int i = 0; i < (int) timeSorted.size(); i++)
        {
            PopupMenu options;
            options.addItem (INDEX_EDIT + i, "Edit");
            String key = getKey ("bypass", timeSorted[(size_t) i]);
            bool bypass = getAppProperties().getUserSettings()->getBoolValue (key);
            options.addItem (INDEX_BYPASS + i, "Bypass", true, bypass);
            options.addSeparator();
            options.addItem (INDEX_MOVE_UP + i, "Move Up", i > 0);
            options.addItem (INDEX_MOVE_DOWN + i, "Move Down", i < (int) timeSorted.size() - 1);
            options.addSeparator();
            options.addItem (INDEX_DELETE + i, "Delete");
            menu.addSubMenu (timeSorted[(size_t) i].name, options);
        }
        menu.addSeparator();
        menu.addSectionHeader ("Available Plugins");
        // All plugins
        KnownPluginList::addToMenu (menu, knownPluginList.getTypes(), pluginSortMethod);
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
    startTimer (50);
}

void IconMenu::menuInvocationCallback (int id, IconMenu* im)
{
    // Right click
    if (! im->menuIconLeftClicked)
    {
        if (id == 1)
        {
            im->savePluginStates();
            return JUCEApplication::getInstance()->quit();
        }
        if (id == 2)
        {
            im->deletePluginStates();
            return im->loadActivePlugins();
        }
        if (id == 3)
        {
            String color = getAppProperties().getUserSettings()->getValue ("icon");
            getAppProperties().getUserSettings()->setValue ("icon", color.equalsIgnoreCase ("black") ? "white" : "black");
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
    // Plugins
    if (id > 2)
    {
        // Delete plugin
        if (id >= im->INDEX_DELETE && id < im->INDEX_DELETE + 1000000)
        {
            im->deletePluginStates();

            const int index = id - im->INDEX_DELETE;
            const std::vector<PluginDescription> timeSorted = im->getTimeSortedList();
            const String key = getKey ("order", timeSorted[(size_t) index]);

            // Remove plugin order
            getAppProperties().getUserSettings()->removeValue (key);
            // Remove bypass entry
            getAppProperties().getUserSettings()->removeValue (getKey ("bypass", timeSorted[(size_t) index]));
            getAppProperties().saveIfNeeded();

            // Remove plugin from the active list
            for (const auto& current : im->activePluginList.getTypes())
            {
                if (key.equalsIgnoreCase (getKey ("order", current)))
                {
                    im->activePluginList.removeType (current);
                    break;
                }
            }

            // Save current states
            im->savePluginStates();
            im->loadActivePlugins();
        }
        // Add plugin
        else if (KnownPluginList::getIndexChosenByMenu (im->knownPluginList.getTypes(), id) > -1)
        {
            const auto knownTypes = im->knownPluginList.getTypes();
            PluginDescription plugin = knownTypes[KnownPluginList::getIndexChosenByMenu (knownTypes, id)];
            String key = getKey ("order", plugin);
            int t = (int) time (nullptr);
            getAppProperties().getUserSettings()->setValue (key, t);
            getAppProperties().saveIfNeeded();
            im->activePluginList.addType (plugin);

            im->savePluginStates();
            im->loadActivePlugins();
        }
        // Bypass plugin
        else if (id >= im->INDEX_BYPASS && id < im->INDEX_BYPASS + 1000000)
        {
            const int index = id - im->INDEX_BYPASS;
            const std::vector<PluginDescription> timeSorted = im->getTimeSortedList();
            const String key = getKey ("bypass", timeSorted[(size_t) index]);

            // Toggle bypass flag
            bool bypassed = getAppProperties().getUserSettings()->getBoolValue (key);
            getAppProperties().getUserSettings()->setValue (key, ! bypassed);
            getAppProperties().saveIfNeeded();

            im->savePluginStates();
            im->loadActivePlugins();
        }
        // Show active plugin GUI
        else if (id >= im->INDEX_EDIT && id < im->INDEX_EDIT + 1000000)
        {
            if (auto* const f = im->graph.getNodeForId (NodeID ((uint32) (id - im->INDEX_EDIT + 1))))
                if (auto* const w = PluginWindow::getWindowFor (f, PluginWindow::Normal))
                    w->toFront (true);
        }
        // Move plugin up the list
        else if (id >= im->INDEX_MOVE_UP && id < im->INDEX_MOVE_UP + 1000000)
        {
            im->savePluginStates();
            const std::vector<PluginDescription> timeSorted = im->getTimeSortedList();
            const PluginDescription toMove = timeSorted[(size_t) (id - im->INDEX_MOVE_UP)];
            for (int i = 0; i < (int) timeSorted.size(); i++)
            {
                bool move = getKey ("move", toMove).equalsIgnoreCase (getKey ("move", timeSorted[(size_t) i]));
                getAppProperties().getUserSettings()->setValue (getKey ("order", timeSorted[(size_t) i]), move ? i : i + 1);
                if (move && i > 0)
                    getAppProperties().getUserSettings()->setValue (getKey ("order", timeSorted[(size_t) (i - 1)]), i + 1);
            }
            im->loadActivePlugins();
        }
        // Move plugin down the list
        else if (id >= im->INDEX_MOVE_DOWN && id < im->INDEX_MOVE_DOWN + 1000000)
        {
            im->savePluginStates();
            const std::vector<PluginDescription> timeSorted = im->getTimeSortedList();
            const PluginDescription toMove = timeSorted[(size_t) (id - im->INDEX_MOVE_DOWN)];
            for (int i = 0; i < (int) timeSorted.size(); i++)
            {
                bool move = getKey ("move", toMove).equalsIgnoreCase (getKey ("move", timeSorted[(size_t) i]));
                getAppProperties().getUserSettings()->setValue (getKey ("order", timeSorted[(size_t) i]), move ? i + 2 : i + 1);
                if (move && i + 1 < (int) timeSorted.size())
                {
                    getAppProperties().getUserSettings()->setValue (getKey ("order", timeSorted[(size_t) (i + 1)]), i + 1);
                    i++;
                }
            }
            im->loadActivePlugins();
        }
        // Update menu
        im->startTimer (50);
    }
}

std::vector<PluginDescription> IconMenu::getTimeSortedList()
{
    int time = 0;
    std::vector<PluginDescription> list;
    for (int i = 0; i < activePluginList.getNumTypes(); i++)
        list.push_back (getNextPluginOlderThanTime (time));
    return list;
}

String IconMenu::getKey (String type, PluginDescription plugin)
{
    String key = "plugin-" + type.toLowerCase() + "-" + plugin.name + plugin.version + plugin.pluginFormatName;
    return key;
}

void IconMenu::deletePluginStates()
{
    const std::vector<PluginDescription> list = getTimeSortedList();
    for (const auto& plugin : list)
    {
        String pluginUid = getKey ("state", plugin);
        getAppProperties().getUserSettings()->removeValue (pluginUid);
        getAppProperties().saveIfNeeded();
    }
}

void IconMenu::savePluginStates()
{
    const std::vector<PluginDescription> list = getTimeSortedList();
    for (int i = 0; i < (int) list.size(); i++)
    {
        auto node = graph.getNodeForId (NodeID ((uint32) (i + 1)));
        if (node == nullptr)
            break;
        AudioProcessor& processor = *node->getProcessor();
        String pluginUid = getKey ("state", list[(size_t) i]);
        MemoryBlock savedStateBinary;
        processor.getStateInformation (savedStateBinary);
        getAppProperties().getUserSettings()->setValue (pluginUid, savedStateBinary.toBase64Encoding());
        getAppProperties().saveIfNeeded();
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

void IconMenu::removePluginsLackingInputOutput()
{
    // NOTE: channel counts in PluginDescription are unreliable for VST3;
    // this preserves upstream behaviour and will be replaced by the graph model.
    for (const auto& plugin : knownPluginList.getTypes())
        if (plugin.numInputChannels < 2 || plugin.numOutputChannels < 2)
            knownPluginList.removeType (plugin);
}
