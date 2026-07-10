#include "MainComponent.h"

namespace lighthost::ui
{

MainComponent::MainComponent (GraphController& controllerToPoll, const PresetStore& presetStoreToRead, Callbacks cb)
    : controller (controllerToPoll), presetStore (presetStoreToRead),
      callbacks (std::move (cb)), canvas (controller)
{
    addAndMakeVisible (titleBar);
    titleBar.onPreferences = [this] { if (callbacks.preferences) callbacks.preferences(); };
    titleBar.onEditPlugins = [this] { if (callbacks.editPlugins) callbacks.editPlugins(); };

    // Preset tabs read live from the store; mutations route up to IconMenu.
    titleBar.getPresetCount  = [this] { return presetStore.getNumPresets(); };
    titleBar.getPresetName   = [this] (int i) { return presetStore.getPresetName (i); };
    titleBar.getActivePreset = [this] { return presetStore.getActiveIndex(); };
    titleBar.onSelectPreset  = [this] (int i) { if (callbacks.selectPreset) callbacks.selectPreset (i); };
    titleBar.onAddPreset     = [this] { if (callbacks.addPreset) callbacks.addPreset(); };
    titleBar.onRenamePreset  = [this] (int i) { if (callbacks.renamePreset) callbacks.renamePreset (i); };
    titleBar.onSavePreset    = [this] { if (callbacks.savePreset) callbacks.savePreset(); };
    titleBar.onDeletePreset  = [this] { if (callbacks.deletePreset) callbacks.deletePreset(); };
    titleBar.refreshPresets();

    canvas.onAddPlugin  = [this] (juce::Point<int> p) { if (callbacks.addPlugin)  callbacks.addPlugin (p); };
    canvas.onOpenEditor = [this] (const juce::String& uid) { if (callbacks.openEditor) callbacks.openEditor (uid); };

    viewport.setViewedComponent (&canvas, false);
    viewport.setScrollBarsShown (true, true);     // free 2D placement: both axes
    addAndMakeVisible (viewport);

    setSize (1200, 720);
    startTimerHz (60);
}

MainComponent::~MainComponent()
{
    stopTimer();
}

void MainComponent::refreshChain()
{
    canvas.refresh();
}

void MainComponent::refreshPresets()
{
    titleBar.refreshPresets();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (LightHostLookAndFeel::appBg());
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    titleBar.setBounds (r.removeFromTop (TitleBar::barHeight));
    viewport.setBounds (r);
    canvas.setVisibleArea (viewport.getMaximumVisibleWidth(), viewport.getMaximumVisibleHeight());
}

void MainComponent::timerCallback()
{
    canvas.tick();
}

} // namespace lighthost::ui
