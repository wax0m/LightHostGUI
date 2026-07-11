//
//  MixProcessor.h — per-node dry/wet mix. Unlike gain/pan (a scalar on the plugin
//  output, done in NodeStripProcessor), a dry/wet blend needs BOTH the pre-plugin
//  (dry) and post-plugin (wet) signal, so GraphController splices this as a summing
//  node fed by two parallel branches:
//
//      [sources] --------> plugin --wet--> MixProcessor(0,1) --> strip --> ...
//           \-----------------------dry--> MixProcessor(2,3) --/
//
//  Bus layout is 4-in / 2-out: input channels 0,1 = WET (plugin output),
//  input channels 2,3 = DRY (the plugin's own input, tapped in parallel). The
//  output is the linear blend  out = (1 - mix) * dry + mix * wet  per channel.
//
//  mix is a live relaxed atomic (message thread writes, audio thread reads); its
//  persisted value lives in GraphDocument. Runtime-only node, like the meters and
//  strips — never part of the persisted document. Header-only.
//
//  Latency: the wet branch carries the plugin's processing latency; the dry branch
//  does not. JUCE's AudioProcessorGraph performs automatic latency compensation
//  across parallel paths, so it delays the dry branch to align the two before they
//  meet here — no manual delay line needed.
//
//  Law is LINEAR (mix 0.5 = half dry + half wet), matching the project's existing
//  linear gain/pan conventions and keeping the blend predictable/testable. (An
//  equal-power sin/cos crossfade would trade predictability for constant perceived
//  loudness on correlated signals; not used.)
//

#pragma once

#include <JuceHeader.h>
#include <atomic>

class MixProcessor : public juce::AudioProcessor
{
public:
    MixProcessor()
        : juce::AudioProcessor (BusesProperties()
              .withInput  ("Wet", juce::AudioChannelSet::stereo(), true)
              .withInput  ("Dry", juce::AudioChannelSet::stereo(), true)
              .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override { return "Mix"; }

    // Message thread. 0 = fully dry, 1 = fully wet.
    void  setMix (float m) noexcept { mix.store (juce::jlimit (0.0f, 1.0f, m), std::memory_order_relaxed); }
    float getMix() const noexcept   { return mix.load (std::memory_order_relaxed); }

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int n = buffer.getNumSamples();
        const float m = mix.load (std::memory_order_relaxed);
        const float dryGain = 1.0f - m;

        // Channels 0,1 hold WET; 2,3 hold DRY. Blend into the output (0,1).
        // Guard the channel count in case the host hands us a smaller buffer.
        const int chans = buffer.getNumChannels();
        for (int outCh = 0; outCh < 2 && outCh < chans; ++outCh)
        {
            const int dryCh = outCh + 2;
            buffer.applyGain (outCh, 0, n, m);                              // wet *= mix
            if (dryCh < chans)
                buffer.addFrom (outCh, 0, buffer, dryCh, 0, n, dryGain);    // += dry * (1-mix)
        }
    }

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
    std::atomic<float> mix { 1.0f };   // default fully wet = current behaviour

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixProcessor)
};
