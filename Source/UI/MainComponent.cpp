#include "MainComponent.h"

namespace lighthost::ui
{

MainComponent::MainComponent (GraphController& controllerToPoll)
    : controller (controllerToPoll)
{
    title.setText ("Light Host", juce::dontSendNotification);
    title.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
    title.setColour (juce::Label::textColourId, LightHostLookAndFeel::text());
    title.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (title);

    addAndMakeVisible (inMeter);
    addAndMakeVisible (outMeter);

    setSize (420, 360);
    startTimerHz (60);
}

MainComponent::~MainComponent()
{
    stopTimer();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (LightHostLookAndFeel::bg());

    auto header = getLocalBounds().removeFromTop (48);
    g.setColour (LightHostLookAndFeel::outline());
    g.fillRect (header.removeFromBottom (1));
}

void MainComponent::resized()
{
    auto r = getLocalBounds();

    title.setBounds (r.removeFromTop (48).reduced (16, 0));

    auto meters = r.reduced (16);
    const int gap = 16;
    const int w   = (meters.getWidth() - gap) / 2;

    inMeter .setBounds (meters.removeFromLeft (w));
    meters.removeFromLeft (gap);
    outMeter.setBounds (meters);
}

void MainComponent::timerCallback()
{
    // Re-fetch each tick: the tap pointers are replaced on every graph rebuild,
    // and are null before the first load or if an IO node is absent.
    feed (inMeter,  controller.getInputMeter());
    feed (outMeter, controller.getOutputMeter());
}

void MainComponent::feed (MeterComponent& meter, const MeterTap* tap)
{
    if (tap != nullptr)
        meter.setLevels (tap->read());
    else
        meter.setLevels ({});   // decay to silence when there is no live tap
}

} // namespace lighthost::ui
