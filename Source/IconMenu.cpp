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
#include "UI/MainWindow.h"
#if JUCE_WINDOWS
#include "Windows.h"
#endif

// Left-click menu id for "Show Editor Window". Safe: the fixed items use 1/2,
// the per-plugin submenus use INDEX_* (>= 1,000,000), and KnownPluginList's
// available-plugins ids start at 0x324503f4 — nothing else claims 3.
static constexpr int kShowWindowMenuId = 3;

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
    // Plugins - known list
    auto savedPluginList = getAppProperties().getUserSettings()->getXmlValue ("pluginList");
    if (savedPluginList != nullptr)
        knownPluginList.recreateFromXml (*savedPluginList);
    pluginSortMethod = KnownPluginList::sortByManufacturer;
    knownPluginList.addChangeListener (this);
    // Signal graph (migrates legacy chain settings on first run)
    controller.loadFrom (*getAppProperties().getUserSettings());
    setIcon();
    setIconTooltip (JUCEApplication::getInstance()->getApplicationName());
}

IconMenu::~IconMenu()
{
    controller.save (*getAppProperties().getUserSettings());
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
        menu.addSeparator();
        menu.addSectionHeader ("Active Plugins");
        // Active plugins
        const auto chain = controller.getChain();
        for (int i = 0; i < (int) chain.size(); i++)
        {
            const auto& item = chain[(size_t) i];
            PopupMenu options;
            options.addItem (INDEX_EDIT + i, "Edit", ! item.missing);
            options.addItem (INDEX_BYPASS + i, "Bypass", true, item.bypassed);
            options.addSeparator();
            options.addItem (INDEX_MOVE_UP + i, "Move Up", i > 0);
            options.addItem (INDEX_MOVE_DOWN + i, "Move Down", i < (int) chain.size() - 1);
            options.addSeparator();
            options.addItem (INDEX_DELETE + i, "Delete");
            menu.addSubMenu (item.missing ? item.name + " (missing)" : item.name, options);
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
    auto& settings = *getAppProperties().getUserSettings();

    // Right click
    if (! im->menuIconLeftClicked)
    {
        if (id == 1)
        {
            im->controller.save (settings);
            return JUCEApplication::getInstance()->quit();
        }
        if (id == 2)
        {
            im->controller.clearAllPluginStates();
            im->controller.save (settings);
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
    // Plugins
    if (id > 2)
    {
        const auto chain = im->controller.getChain();
        const auto chainUidForId = [&chain] (int itemId, int base) -> String
        {
            const int index = itemId - base;
            if (index >= 0 && index < (int) chain.size())
                return chain[(size_t) index].uid;
            return {};
        };

        // Delete plugin
        if (id >= im->INDEX_DELETE && id < im->INDEX_DELETE + 1000000)
        {
            if (const String uid = chainUidForId (id, im->INDEX_DELETE); uid.isNotEmpty())
            {
                im->controller.removeFromChain (uid);
                im->controller.save (settings);
            }
        }
        // Add plugin
        else if (KnownPluginList::getIndexChosenByMenu (im->knownPluginList.getTypes(), id) > -1)
        {
            const auto knownTypes = im->knownPluginList.getTypes();
            const PluginDescription plugin = knownTypes[KnownPluginList::getIndexChosenByMenu (knownTypes, id)];
            im->controller.appendToChain (plugin);
            im->controller.save (settings);
        }
        // Bypass plugin (live pass-through, no rebuild)
        else if (id >= im->INDEX_BYPASS && id < im->INDEX_BYPASS + 1000000)
        {
            const int index = id - im->INDEX_BYPASS;
            if (index >= 0 && index < (int) chain.size())
            {
                const auto& item = chain[(size_t) index];
                im->controller.setBypassed (item.uid, ! item.bypassed);
                im->controller.save (settings);
            }
        }
        // Show active plugin GUI
        else if (id >= im->INDEX_EDIT && id < im->INDEX_EDIT + 1000000)
        {
            if (const String uid = chainUidForId (id, im->INDEX_EDIT); uid.isNotEmpty())
                if (auto* const node = im->controller.getNodeForUid (uid))
                    if (auto* const w = PluginWindow::getWindowFor (node, PluginWindow::Normal))
                        w->toFront (true);
        }
        // Move plugin up the list
        else if (id >= im->INDEX_MOVE_UP && id < im->INDEX_MOVE_UP + 1000000)
        {
            if (const String uid = chainUidForId (id, im->INDEX_MOVE_UP); uid.isNotEmpty())
            {
                im->controller.moveUp (uid);
                im->controller.save (settings);
            }
        }
        // Move plugin down the list
        else if (id >= im->INDEX_MOVE_DOWN && id < im->INDEX_MOVE_DOWN + 1000000)
        {
            if (const String uid = chainUidForId (id, im->INDEX_MOVE_DOWN); uid.isNotEmpty())
            {
                im->controller.moveDown (uid);
                im->controller.save (settings);
            }
        }
        // Update menu
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
        mainWindow = std::make_unique<lighthost::ui::MainWindow> (controller);

    #if JUCE_MAC
    Process::setDockIconVisible (true);
    #endif
    mainWindow->setVisible (true);
    mainWindow->toFront (true);
}

void IconMenu::removePluginsLackingInputOutput()
{
    // NOTE: channel counts in PluginDescription are unreliable for VST3;
    // this preserves upstream behaviour and will be revisited with the GUI.
    for (const auto& plugin : knownPluginList.getTypes())
        if (plugin.numInputChannels < 2 || plugin.numOutputChannels < 2)
            knownPluginList.removeType (plugin);
}
