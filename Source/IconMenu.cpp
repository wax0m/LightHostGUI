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
static constexpr int kMonoInputMenuId  = 6;   // checkable "Mono Input" toggle

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

IconMenu::IconMenu() : INDEX_EDIT (1000000), INDEX_BYPASS (2000000), INDEX_DELETE (3000000), INDEX_MOVE_UP (4000000), INDEX_MOVE_DOWN (5000000), INDEX_PRESET (6000000)
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
    // MIDI inputs -> player -> graph's midiInputNode (M7). The player is a
    // MidiInputCallback and forwards MIDI into the graph it drives.
    for (auto& mi : MidiInput::getAvailableDevices())
        deviceManager.setMidiInputDeviceEnabled (mi.identifier, true);
    deviceManager.addMidiInputDeviceCallback ({}, &player);   // {} = all enabled inputs
    // Plugins - known list
    auto savedPluginList = getAppProperties().getUserSettings()->getXmlValue ("pluginList");
    if (savedPluginList != nullptr)
        knownPluginList.recreateFromXml (*savedPluginList);
    pluginSortMethod = KnownPluginList::sortByManufacturer;
    knownPluginList.addChangeListener (this);
    // Signal graph (migrates legacy chain settings on first run)
    controller.loadFrom (*getAppProperties().getUserSettings());
    // Mono-input preference (app-global; sums a mono source to both channels).
    controller.setMonoInput (getAppProperties().getUserSettings()->getBoolValue ("monoInput", false));
    // Presets: load the store, or seed a "Default" from the just-loaded graph.
    loadPresets();
    setIcon();
    setIconTooltip (JUCEApplication::getInstance()->getApplicationName());
}

IconMenu::~IconMenu()
{
    deviceManager.removeMidiInputDeviceCallback ({}, &player);
    persistPresets();
    controller.save (*getAppProperties().getUserSettings());
}

//== Presets ===================================================================
void IconMenu::loadPresets()
{
    auto& settings = *getAppProperties().getUserSettings();
    store = PresetStore::fromXml (settings.getValue ("presets"));

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
    if (name.isEmpty())
        return;

    // Snapshot the current scene, then add a copy of it as a new active preset.
    if (const int active = store.getActiveIndex(); active >= 0)
        store.setPresetDocument (active, controller.snapshotDocument());
    const int idx = store.addPreset (name, controller.snapshotDocument());
    store.setActiveIndex (idx);   // live doc already equals the new preset — no reload

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

void IconMenu::deleteActivePreset()
{
    const int active = store.getActiveIndex();
    if (active < 0)
        return;

    store.removePreset (active);
    if (store.getNumPresets() == 0)   // never leave 0 presets
    {
        store.addPreset ("Default", controller.snapshotDocument());
        store.setActiveIndex (0);
    }

    const int newActive = store.getActiveIndex();
    controller.loadDocument (store.getPresetDocument (newActive));

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
        menu.addItem (kMonoInputMenuId, "Mono Input", true, controller.isMonoInput());
        // Presets switcher — change the active scene from the background.
        {
            PopupMenu presets;
            for (int i = 0; i < store.getNumPresets(); ++i)
                presets.addItem (INDEX_PRESET + i, store.getPresetName (i), true, i == store.getActiveIndex());
            menu.addSubMenu ("Presets", presets);
        }
        menu.addSeparator();
        menu.addSectionHeader ("Active Plugins");
        // The serial-chain ops (add/delete/move) rewrite the graph as a straight
        // chain, which would clobber parallel routing made on the canvas — so
        // they are only offered while the graph is still a linear chain.
        const bool linear = controller.isLinearChain();
        if (! linear)
            menu.addItem (4, "Chain edits disabled: graph has parallel routing - edit on the canvas.", false);
        // Active plugins
        const auto chain = controller.getChain();
        for (int i = 0; i < (int) chain.size(); i++)
        {
            const auto& item = chain[(size_t) i];
            PopupMenu options;
            options.addItem (INDEX_EDIT + i, "Edit", ! item.missing);
            options.addItem (INDEX_BYPASS + i, "Bypass", true, item.bypassed);
            options.addSeparator();
            options.addItem (INDEX_MOVE_UP + i, "Move Up", linear && i > 0);
            options.addItem (INDEX_MOVE_DOWN + i, "Move Down", linear && i < (int) chain.size() - 1);
            options.addSeparator();
            options.addItem (INDEX_DELETE + i, "Delete", linear);
            menu.addSubMenu (item.missing ? item.name + " (missing)" : item.name, options);
        }
        menu.addSeparator();
        menu.addSectionHeader ("Available Plugins");
        // All plugins (adding appends serially, so it is gated like the other chain ops)
        if (linear)
            KnownPluginList::addToMenu (menu, knownPluginList.getTypes(), pluginSortMethod);
        else
            menu.addItem (5, "Chain edits disabled: graph has parallel routing - edit on the canvas.", false);
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
            im->persistPresets();
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
    // Mono Input toggle (sum input to both channels). Persist + refresh the tick.
    if (id == kMonoInputMenuId)
    {
        const bool on = ! im->controller.isMonoInput();
        im->controller.setMonoInput (on);
        settings.setValue ("monoInput", on);
        settings.saveIfNeeded();
        return im->startTimer (50);
    }
    // Preset switch (from the tray Presets submenu). Handled first so it never
    // falls through to the serial chain-op decoding below.
    if (id >= im->INDEX_PRESET && id < im->INDEX_PRESET + 1000000)
        return im->switchToPreset (id - im->INDEX_PRESET);
    // Plugins
    if (id > 2)
    {
        // Re-checked at invocation time: the menu may have been built before a
        // canvas edit branched the graph, and the serial ops must never run then.
        const bool linear = im->controller.isLinearChain();
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
            if (const String uid = chainUidForId (id, im->INDEX_DELETE); uid.isNotEmpty() && linear)
            {
                im->controller.removeFromChain (uid);
                im->controller.save (settings);
            }
        }
        // Add plugin
        else if (KnownPluginList::getIndexChosenByMenu (im->knownPluginList.getTypes(), id) > -1)
        {
            if (linear)
            {
                const auto knownTypes = im->knownPluginList.getTypes();
                const PluginDescription plugin = knownTypes[KnownPluginList::getIndexChosenByMenu (knownTypes, id)];
                im->controller.appendToChain (plugin);
                im->controller.save (settings);
            }
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
            if (const String uid = chainUidForId (id, im->INDEX_MOVE_UP); uid.isNotEmpty() && linear)
            {
                im->controller.moveUp (uid);
                im->controller.save (settings);
            }
        }
        // Move plugin down the list
        else if (id >= im->INDEX_MOVE_DOWN && id < im->INDEX_MOVE_DOWN + 1000000)
        {
            if (const String uid = chainUidForId (id, im->INDEX_MOVE_DOWN); uid.isNotEmpty() && linear)
            {
                im->controller.moveDown (uid);
                im->controller.save (settings);
            }
        }
        // Reflect any chain change in the open editor window.
        if (im->mainWindow != nullptr)
            im->mainWindow->refreshChain();
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
    {
        lighthost::ui::MainComponent::Callbacks cb;
        cb.addPlugin    = [this] (juce::Point<int> p) { showAddPluginMenu (p); };
        cb.openEditor   = [this] (const String& uid)  { openEditorForUid (uid); };
        cb.preferences  = [this] { showAudioSettings(); };
        cb.editPlugins  = [this] { reloadPlugins(); };
        cb.selectPreset = [this] (int i) { switchToPreset (i); };
        cb.addPreset    = [this] { addPreset(); };
        cb.renamePreset = [this] (int i) { renamePreset (i); };
        cb.savePreset   = [this] { saveActivePreset(); };
        cb.deletePreset = [this] { deleteActivePreset(); };
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
}

void IconMenu::showAddPluginMenu (juce::Point<int> screenPos)
{
    PopupMenu m;
    KnownPluginList::addToMenu (m, knownPluginList.getTypes(), pluginSortMethod);

    m.showMenuAsync (PopupMenu::Options()
                        .withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
        [this] (int r)
        {
            if (r <= 0)
                return;
            const auto types = knownPluginList.getTypes();
            const int idx = KnownPluginList::getIndexChosenByMenu (types, r);
            if (idx < 0)
                return;
            // Serial append rewrites ALL connections as a chain, which would destroy
            // parallel routing — on a branched graph add the node unconnected instead
            // (the user wires it on the canvas). Checked here, not at menu-build time.
            if (controller.isLinearChain())
                controller.appendToChain (types[idx]);
            else
                controller.addNodeUnconnected (types[idx]);
            controller.save (*getAppProperties().getUserSettings());
            if (mainWindow != nullptr)
                mainWindow->refreshChain();
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
    for (const auto& plugin : knownPluginList.getTypes())
        if (plugin.numInputChannels < 2 || plugin.numOutputChannels < 2)
            knownPluginList.removeType (plugin);
}
