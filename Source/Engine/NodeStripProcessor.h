//
//  NodeStripProcessor.h — per-node output stage: linear output gain + stereo
//  balance (pan), then a meter tap on the result. GraphController splices one
//  after each plugin node. Gain/pan are set live from the message thread
//  (relaxed atomics; the audio thread only reads); their persisted values live
//  in GraphDocument. The meter is runtime-only, like MeterProcessor.
//
//  Pan law: linear stereo balance. pan 0 = unity both channels; pan +1 fully
//  right (left muted); pan -1 fully left (right muted).
//

#pragma once

#include <JuceHeader.h>
#include "MeterTap.h"
#include <atomic>

class NodeStripProcessor : public juce::AudioProcessor
{
public:
    NodeStripProcessor()
        : juce::AudioProcessor (BusesProperties()
              .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override { return "NodeStrip"; }

    // Message thread.
    void  setGain (float g) noexcept { gain.store (g, std::memory_order_relaxed); }
    void  setPan  (float p) noexcept { pan.store (juce::jlimit (-1.0f, 1.0f, p), std::memory_order_relaxed); }
    float getGain() const noexcept   { return gain.load (std::memory_order_relaxed); }
    float getPan()  const noexcept   { return pan.load (std::memory_order_relaxed); }

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const float g = gain.load (std::memory_order_relaxed);
        const float p = pan.load  (std::memory_order_relaxed);
        const float lGain = g * (p <= 0.0f ? 1.0f : 1.0f - p);
        const float rGain = g * (p >= 0.0f ? 1.0f : 1.0f + p);

        const int n = buffer.getNumSamples();
        if (buffer.getNumChannels() > 0) buffer.applyGain (0, 0, n, lGain);
        if (buffer.getNumChannels() > 1) buffer.applyGain (1, 0, n, rGain);

        tap.measureBlock (buffer);   // post gain/pan: this is the node's output level
    }

    const MeterTap& getTap() const noexcept { return tap; }

    //==============================================================================
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
    std::atomic<float> gain { 1.0f };
    std::atomic<float> pan  { 0.0f };
    MeterTap tap;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NodeStripProcessor)
};
