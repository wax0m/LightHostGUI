//
//  MonoInputProcessor.h — sums the stereo input to mono and writes it to BOTH
//  channels. GraphController splices one right after the audio-input node when
//  "Mono Input" is enabled, so a mono source that arrives on only one channel
//  (e.g. a mic wired to the right input) is centred and heard on both outputs.
//
//  Runtime-only, like MeterProcessor: never part of the persisted GraphDocument
//  (the on/off flag lives in app settings). Header-only.
//
//  Sum (L+R), not average: for a one-sided source the silent channel contributes
//  nothing, so the surviving channel keeps its full level on both outputs.
//

#pragma once

#include <JuceHeader.h>

class MonoInputProcessor : public juce::AudioProcessor
{
public:
    MonoInputProcessor()
        : juce::AudioProcessor (BusesProperties()
              .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override { return "MonoInput"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (buffer.getNumChannels() < 2)
            return;   // nothing to fold

        const int n = buffer.getNumSamples();
        auto* l = buffer.getWritePointer (0);
        auto* r = buffer.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const float m = l[i] + r[i];
            l[i] = m;
            r[i] = m;
        }
    }

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
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MonoInputProcessor)
};
