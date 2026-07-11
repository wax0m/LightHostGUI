//
//  PassThroughProcessor.h — stereo identity processor. The controller substitutes
//  one of these for a plugin that FAILS TO LOAD, so a missing plugin does not sever
//  the signal chain — audio flows through its slot unchanged (the node card still
//  shows "missing"). Header-only.
//

#pragma once

#include <JuceHeader.h>

class PassThroughProcessor : public juce::AudioProcessor
{
public:
    PassThroughProcessor()
        : juce::AudioProcessor (BusesProperties()
              .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override { return "Missing"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}   // identity: leave the buffer
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PassThroughProcessor)
};
