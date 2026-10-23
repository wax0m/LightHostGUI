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
#include "UI/MainWindow.h"
#if JUCE_WINDOWS
#include "Windows.h"
#endif

// Left-click menu id for "Show Editor Window". Safe: the fixed items use 1/2,
// the preset items use INDEX_PRESET (>= 6,000,000), and KnownPluginList's
// available-plugins ids start at 0x324503f4 — nothing else claims 3.
static constexpr int kShowWindowMenuId = 3;
static constexpr int kMonoInputMenuId  = 6;   // checkable "Mono Input" toggle
static constexpr int kRetryMissingMenuId = 7; // "Retry missing plugins (N)", shown only when N > 0

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

IconMenu::IconMenu (juce::String startupWarning) : INDEX_PRESET (6000000)
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

    // A v1.0 build would read a v2 graph as px values clamped to 1 (every node bottom-right), so keep the
    // v1 file once, before the first save can rewrite it. No dialog: the migration itself is lossless.
    if (controller.didMigrateOnLoad() || store.migratedOnLoad)
        lighthost::copySettingsFileAside (*getAppProperties().getUserSettings(), "v1");

    // One non-modal notice for whatever could not be read (the backups are already written).
    const auto warning = (startupWarning + "\n\n" + controller.getLoadWarning() + "\n\n" + presetsWarning).trim();
    if (warning.isNotEmpty())
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Light Host", warning);

    setIcon();
    setIconTooltip (JUCEApplication::getInstance()->getApplicationName());
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
    if (id > 2 && KnownPluginList::getIndexChosenByMenu (im->knownPluginList.getTypes(), id) > -1)
    {
        const auto knownTypes = im->knownPluginList.getTypes();
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
            // addPlugin appends serially on a linear chain and adds unconnected on a
            // branched graph (never linearizes parallel routing) — decided in the
            // controller so every add entry point shares the policy.
            const String uid = controller.addPlugin (types[idx]);
            controller.save (*getAppProperties().getUserSettings());
            if (mainWindow != nullptr)
            {
                mainWindow->refreshChain();
                mainWindow->revealNode (uid);   // the floating ADD may be far from the new node
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
    for (const auto& plugin : knownPluginList.getTypes())
        if (plugin.numInputChannels < 2 || plugin.numOutputChannels < 2)
            knownPluginList.removeType (plugin);
}
