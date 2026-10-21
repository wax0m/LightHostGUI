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
    titleBar.getSampleRate   = callbacks.sampleRate;
    titleBar.getCpuLoad      = callbacks.cpuLoad;
    titleBar.refreshPresets();

    canvas.onOpenEditor = [this] (const juce::String& uid) { if (callbacks.openEditor) callbacks.openEditor (uid); };
    canvas.onZoomBy     = [this] (float f, std::optional<juce::Point<int>> a) { zoomBy (f, a); };
    canvas.onZoomReset  = [this] { setZoom (1.0f, std::nullopt); };
    canvas.onZoomFit    = [this] { zoomToFit(); };

    viewport.setViewedComponent (&zoomHolder, false);
    viewport.setScrollBarsShown (true, true);     // free 2D placement: both axes
    addAndMakeVisible (viewport);

    // Floating chrome: added after the viewport so it paints above it and never scrolls.
    addButton.onClick = [this]
    {
        if (callbacks.addPlugin)
            callbacks.addPlugin (addButton.localPointToGlobal (addButton.getLocalBounds().getBottomLeft()));
    };
    addAndMakeVisible (addButton);

    zoomControls.onZoomOut = [this] { zoomBy (1.0f / CanvasView::kZoomStep, std::nullopt); };
    zoomControls.onZoomIn  = [this] { zoomBy (CanvasView::kZoomStep, std::nullopt); };
    zoomControls.onReset   = [this] { setZoom (1.0f, std::nullopt); };
    zoomControls.onFit     = [this] { zoomToFit(); };
    addAndMakeVisible (zoomControls);

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

void MainComponent::revealNode (const juce::String& uid)
{
    const auto logical = canvas.getNodeBounds (uid);
    if (logical.isEmpty())
        return;

    const auto scaled   = (logical.toFloat() * zoomHolder.getZoom()).getSmallestIntegerContainer();
    const auto viewArea = viewport.getViewArea();
    if (viewArea.contains (scaled))
        return;

    viewport.setViewPosition (scaled.getCentreX() - viewArea.getWidth()  / 2,
                              scaled.getCentreY() - viewArea.getHeight() / 2);
}

//== Zoom ======================================================================
void MainComponent::zoomBy (float factor, std::optional<juce::Point<int>> screenAnchor)
{
    setZoom (zoomHolder.getZoom() * factor, screenAnchor);
}

void MainComponent::setZoom (float newZoom, std::optional<juce::Point<int>> screenAnchor)
{
    newZoom = juce::jlimit (CanvasView::kMinZoom, CanvasView::kMaxZoom, newZoom);
    const float oldZoom = zoomHolder.getZoom();
    if (std::abs (newZoom - oldZoom) < 1.0e-4f)
        return;

    // Keep the canvas point under the anchor (mouse position, or the view centre) fixed.
    const juce::Point<int> anchor = screenAnchor.has_value()
        ? viewport.getLocalPoint (nullptr, *screenAnchor)
        : juce::Point<int> (viewport.getViewWidth() / 2, viewport.getViewHeight() / 2);
    const auto view = viewport.getViewPosition();
    const float lx = (float) (view.x + anchor.x) / oldZoom;
    const float ly = (float) (view.y + anchor.y) / oldZoom;

    applyZoom (newZoom);
    viewport.setViewPosition (juce::roundToInt (lx * newZoom) - anchor.x,
                              juce::roundToInt (ly * newZoom) - anchor.y);
}

void MainComponent::zoomToFit()
{
    const auto content = canvas.getContentBounds().expanded (40);
    if (content.isEmpty())
        return;

    const float z = juce::jlimit (CanvasView::kMinZoom, CanvasView::kMaxZoom,
                                  juce::jmin ((float) viewport.getViewWidth()  / (float) content.getWidth(),
                                              (float) viewport.getViewHeight() / (float) content.getHeight()));
    applyZoom (z);
    viewport.setViewPosition (juce::roundToInt ((float) content.getCentreX() * z) - viewport.getViewWidth()  / 2,
                              juce::roundToInt ((float) content.getCentreY() * z) - viewport.getViewHeight() / 2);
}

void MainComponent::applyZoom (float z)
{
    zoomHolder.setZoom (z);
    canvas.setViewportSize (viewport.getMaximumVisibleWidth(), viewport.getMaximumVisibleHeight(), z);
    zoomControls.setZoom (z);
    layoutOverlays();   // scrollbars may have appeared or vanished
}

void MainComponent::layoutOverlays()
{
    const auto vp = viewport.getBounds();
    addButton.setBounds (vp.getX() + 16, vp.getY() + 16, 70, 60);
    zoomControls.setBounds (vp.getX() + viewport.getViewWidth()  - ZoomControls::preferredWidth  - 14,
                            vp.getY() + viewport.getViewHeight() - ZoomControls::preferredHeight - 14,
                            ZoomControls::preferredWidth, ZoomControls::preferredHeight);
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
    canvas.setViewportSize (viewport.getMaximumVisibleWidth(), viewport.getMaximumVisibleHeight(), zoomHolder.getZoom());
    layoutOverlays();
}

void MainComponent::setUiTimerRunning (bool shouldRun)
{
    if (shouldRun && ! isTimerRunning())
        startTimerHz (60);
    else if (! shouldRun)
        stopTimer();
}

void MainComponent::timerCallback()
{
    canvas.tick();

    // Title-bar CPU/sample-rate readout: ~2 Hz is plenty for text/a load bar.
    if (++statsTickCounter >= 30)
    {
        statsTickCounter = 0;
        titleBar.repaintStats();   // stats cluster only — tabs/logo don't change
    }
}

} // namespace lighthost::ui
