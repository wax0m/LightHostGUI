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

    static constexpr int barHeight = 50;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    void showSettingsMenu();

    juce::StringArray tabs { "Vocal Chain", "Drum Bus", "Master", "Guitar DI", "Synth Wide" };
    int activeTab = 0;

    juce::Array<juce::Rectangle<int>> tabRects;
    juce::Rectangle<int> settingsRect;
    bool settingsHot = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TitleBar)
};

} // namespace lighthost::ui
