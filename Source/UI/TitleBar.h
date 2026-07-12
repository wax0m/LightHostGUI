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

class TitleBar : public juce::Component
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
    std::function<void()>                onSavePreset;
    std::function<void()>                onDeletePreset;

    // Live device stats (supplied by IconMenu via MainComponent). Null / <= 0 means
    // unknown — the readout shows a placeholder rather than a made-up number.
    std::function<double()>              getSampleRate;   // Hz
    std::function<double()>              getCpuLoad;      // 0..1

    void refreshPresets();   // recompute tab layout + repaint from the store

    static constexpr int barHeight = 50;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    void showSettingsMenu();
    juce::StringArray presetNames() const;

    juce::Array<juce::Rectangle<int>> tabRects;
    juce::Rectangle<int> plusRect;
    juce::Rectangle<int> settingsRect;
    bool settingsHot = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TitleBar)
};

} // namespace lighthost::ui
