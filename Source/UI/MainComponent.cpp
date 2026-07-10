#include "MainComponent.h"

namespace lighthost::ui
{

MainComponent::MainComponent (GraphController& controllerToPoll, Callbacks cb)
    : controller (controllerToPoll), callbacks (std::move (cb)), canvas (controller)
{
    addAndMakeVisible (titleBar);
    titleBar.onPreferences = [this] { if (callbacks.preferences) callbacks.preferences(); };
    titleBar.onEditPlugins = [this] { if (callbacks.editPlugins) callbacks.editPlugins(); };

    canvas.onAddPlugin  = [this] (juce::Point<int> p) { if (callbacks.addPlugin)  callbacks.addPlugin (p); };
    canvas.onOpenEditor = [this] (const juce::String& uid) { if (callbacks.openEditor) callbacks.openEditor (uid); };

    viewport.setViewedComponent (&canvas, false);
    viewport.setScrollBarsShown (true, true);     // free 2D placement: both axes
    addAndMakeVisible (viewport);

    setSize (900, TitleBar::barHeight + CanvasView::canvasHeight);
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

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (LightHostLookAndFeel::appBg());
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    titleBar.setBounds (r.removeFromTop (TitleBar::barHeight));
    viewport.setBounds (r);
}

void MainComponent::timerCallback()
{
    canvas.tick();
}

} // namespace lighthost::ui
