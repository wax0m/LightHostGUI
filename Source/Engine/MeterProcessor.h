//
//  MeterProcessor.h — a stereo pass-through AudioProcessor that measures the
//  signal flowing through it into a MeterTap, without altering it.
//
//  The GraphController splices these into the LIVE graph (around the master IO,
//  and later per node) after each rebuild. They are never part of the persisted
//  GraphDocument — metering is runtime infrastructure, not saved state.
//
//  Header-only so every target that compiles GraphController picks it up with no
//  extra build wiring.
//

#pragma once

#include <JuceHeader.h>
#include "MeterTap.h"

class MeterProcessor : public juce::AudioProcessor
{
public:
    MeterProcessor()
        : juce::AudioProcessor (BusesProperties()
              .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override      { return "Meter"; }

    void prepareToPlay (double, int) override         {}
    void releaseResources() override                  {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        tap.measureBlock (buffer);   // pass-through: buffer is left untouched
    }

    // The tap is read by the GUI on the message thread.
    const MeterTap& getTap() const noexcept           { return tap; }

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override                   { return false; }

    bool acceptsMidi() const override                 { return false; }
    bool producesMidi() const override                { return false; }
    double getTailLengthSeconds() const override      { return 0.0; }

    int getNumPrograms() override                     { return 1; }
    int getCurrentProgram() override                  { return 0; }
    void setCurrentProgram (int) override             {}
    const juce::String getProgramName (int) override  { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override   {}

private:
    MeterTap tap;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeterProcessor)
};
