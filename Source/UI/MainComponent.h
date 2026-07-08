//
//  MainComponent.h — the main window's content. Polls the controller's master
//  meter taps on a 60 fps timer and paints master IN/OUT meters.
//
//  This is the one UI class that touches the engine, and only through
//  GraphController's read-only meter accessors — it never mutates the graph and
//  never calls onto the audio thread. It re-fetches the tap pointer every tick
//  (the pointer changes on each graph rebuild), so it never caches a stale one.
//

#pragma once

#include <JuceHeader.h>
#include "../Engine/GraphController.h"
#include "MeterComponent.h"

namespace lighthost::ui
{

class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    explicit MainComponent (GraphController& controllerToPoll);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    static void feed (MeterComponent&, const MeterTap*);

    GraphController& controller;

    juce::Label     title;
    MeterComponent  inMeter  { "IN"  };
    MeterComponent  outMeter { "OUT" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace lighthost::ui
