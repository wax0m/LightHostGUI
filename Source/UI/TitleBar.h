//
//  TitleBar.h — top strip (design spec §"Title bar"): diamond logo + wordmark,
//  preset tabs (visual only for now), CPU + sample-rate readout, and the ⋯
//  settings menu ("Edit Plug-ins…" wired; "Preferences…" wired to audio setup).
//

#pragma once

#include <JuceHeader.h>
#include "LightHostLookAndFeel.h"

namespace lighthost::ui
{

class TitleBar : public juce::Component, public juce::TooltipClient
{
public:
    TitleBar();

    std::function<void()> onPreferences;
    std::function<void()> onEditPlugins;

    // Preset binding (supplied by MainComponent → PresetStore / IconMenu).
    std::function<int()>                 getPresetCount;
    std::function<juce::String (int)>    getPresetName;
    std::function<int()>                 getActivePreset;
    std::function<void (int)>            onSelectPreset;
    std::function<void()>                onAddPreset;
    std::function<void (int)>            onRenamePreset;
    std::function<void (int)>            onDuplicatePreset;
    std::function<void()>                onSavePreset;
    std::function<void (int)>            onDeletePreset;
    std::function<void (int, int)>       onMovePreset;   // from, final index
    // Global preset shortcuts (Windows). shortcutsAvailable null / false hides the menu items.
    std::function<bool()>                shortcutsAvailable;
    std::function<juce::String (int)>    getPresetShortcut;   // canonical text, "" = none
    std::function<void (int)>            onSetShortcut;
    std::function<void (int)>            onClearShortcut;

    juce::String getTooltip() override;   // the shortcut of the tab under the mouse

    // Live device stats (supplied by IconMenu via MainComponent). Null / <= 0 means
    // unknown — the readout shows a placeholder rather than a made-up number.
    std::function<double()>              getSampleRate;   // Hz
    std::function<double()>              getCpuLoad;      // 0..1

    void refreshPresets();   // recompute tab layout + repaint from the store

    // Repaint only the CPU/sample-rate cluster (for the periodic stats tick) —
    // the tabs/logo/gradient don't change, so don't rasterise the whole bar.
    void repaintStats();

    // The CPU meter + sample-rate cluster, full height. paint() lays the cluster out from it
    // and repaintStats() repaints exactly it, so the two cannot drift apart.
    juce::Rectangle<int> statsClusterBounds() const;

    static constexpr int barHeight = 50;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    void showSettingsMenu();
    void showTabMenu (int tab);   // right click on a preset tab
    juce::StringArray presetNames() const;

    juce::Array<juce::Rectangle<int>> tabRects;
    juce::Rectangle<int> plusRect;
    juce::Rectangle<int> settingsRect;
    bool settingsHot = false;
    int menuTab = -1;

    // A press on a tab selects it on release; dragging it past dragThreshold px reorders instead.
    static constexpr int dragThreshold = 4;
    int  pressedTab = -1;
    bool dragging = false;
    int  dropGap = -1;   // insertion slot 0..n (before tab i), valid while dragging
    int  gapAt (int x) const;
    int  tabAt (juce::Point<int> p) const;
    int  hoverTab = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TitleBar)
};

} // namespace lighthost::ui
