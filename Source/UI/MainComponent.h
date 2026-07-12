//
//  MainComponent.h — the main window's content: title bar + a horizontally
//  scrolling node canvas. Owns the 60 fps timer that advances the canvas meters
//  and signal-flow animation. The one UI class that touches the engine, and only
//  through GraphController's message-thread API — never the audio thread.
//
//  Host-level actions the canvas/title bar cannot do alone (add-plugin picker,
//  open a native editor, preferences, plugin scan) are injected as callbacks by
//  IconMenu so the UI stays decoupled from the tray/host plumbing.
//

#pragma once

#include <JuceHeader.h>
#include "../Engine/GraphController.h"
#include "../Engine/PresetStore.h"
#include "TitleBar.h"
#include "CanvasView.h"

namespace lighthost::ui
{

class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    struct Callbacks
    {
        std::function<void (juce::Point<int> screenPos)> addPlugin;
        std::function<void (const juce::String& uid)>    openEditor;
        std::function<void()>                            preferences;
        std::function<void()>                            editPlugins;
        // Presets (routed to IconMenu, which owns the store + does save/load).
        std::function<void (int)>                        selectPreset;
        std::function<void()>                            addPreset;
        std::function<void (int)>                        renamePreset;
        std::function<void()>                            savePreset;
        std::function<void()>                            deletePreset;
        // Live device stats for the title bar readout.
        std::function<double()>                          sampleRate;   // Hz
        std::function<double()>                          cpuLoad;      // 0..1
    };

    MainComponent (GraphController&, const PresetStore&, Callbacks);
    ~MainComponent() override;

    void refreshChain();     // rebuild cards after an external chain change
    void refreshPresets();   // re-read the preset tabs after an external change

    // The window hides to the tray instead of closing; pause the 60 Hz UI timer
    // while hidden so a backgrounded host does no metering/param-poll work.
    void setUiTimerRunning (bool shouldRun);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    GraphController& controller;
    const PresetStore& presetStore;
    Callbacks callbacks;

    TitleBar       titleBar;
    juce::Viewport viewport;
    CanvasView     canvas;
    int            statsTickCounter = 0;   // throttles the title-bar stats repaint

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace lighthost::ui
