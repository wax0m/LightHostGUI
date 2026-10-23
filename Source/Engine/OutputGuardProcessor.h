//
//  OutputGuardProcessor.h — last node before the audio output: a zero-latency soft
//  safety limiter at -1 dBFS plus a NaN/Inf scrubber. Runtime-only, like MeterProcessor;
//  GraphController splices it in front of the output meter. Not a creative limiter:
//  no look-ahead (LightHost is used for live monitoring, so no added latency), instant
//  attack, ~100 ms release. Below the ceiling the signal passes bit-exact.
//
//  Per sample: sanitise (BEFORE the detector, so a NaN can never poison the envelope)
//  -> linked-stereo peak -> gain envelope -> in * gain -> last-resort hard clamp.
//

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cmath>

class OutputGuardProcessor : public juce::AudioProcessor
{
public:
    OutputGuardProcessor()
        : juce::AudioProcessor (BusesProperties()
              .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override { return "OutputGuard"; }

    static float getCeiling() noexcept { return juce::Decibels::decibelsToGain (-1.0f); }

    // Read-and-reset latches for a future clip/limit indicator (message thread).
    bool consumeLimiting() noexcept  { return limiting.exchange (false, std::memory_order_relaxed); }
    bool consumeNonFinite() noexcept { return nonFinite.exchange (false, std::memory_order_relaxed); }

    void prepareToPlay (double sampleRate, int) override
    {
        releaseCoeff = 1.0 - std::exp (-1.0 / (0.1 * juce::jmax (1.0, sampleRate)));
        gainEnv = 1.0;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;

        const int n = buffer.getNumSamples();
        const int numCh = buffer.getNumChannels();
        if (numCh == 0)
            return;

        const float ceiling = getCeiling();
        float* l = buffer.getWritePointer (0);
        float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

        double g = gainEnv;
        double minG = 1.0;
        bool sawNonFinite = false;

        for (int i = 0; i < n; ++i)
        {
            float a = l[i];
            float b = r != nullptr ? r[i] : 0.0f;

            if (! std::isfinite (a)) { a = 0.0f; sawNonFinite = true; }
            if (! std::isfinite (b)) { b = 0.0f; sawNonFinite = true; }

            const float peak = juce::jmax (std::abs (a), std::abs (b));
            const double t = peak > ceiling ? (double) ceiling / (double) peak : 1.0;

            if (t < g)
                g = t;                                   // instant attack
            else
            {
                g += (t - g) * releaseCoeff;             // smooth release
                if (t == 1.0 && g > 0.9999)
                    g = 1.0;                             // snap, so quiet audio is bit-exact again (the tail is inaudible: < 0.001 dB)
            }
            minG = juce::jmin (minG, g);

            const float gf = (float) g;
            l[i] = juce::jlimit (-ceiling, ceiling, a * gf);
            if (r != nullptr)
                r[i] = juce::jlimit (-ceiling, ceiling, b * gf);
        }

        gainEnv = g;
        if (minG < 0.944)   // > 0.5 dB of gain reduction
            limiting.store (true, std::memory_order_relaxed);
        if (sawNonFinite)
            nonFinite.store (true, std::memory_order_relaxed);
    }

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
    // Double: a float envelope stalls just below 1.0 (the release step drops under half an ulp).
    double gainEnv = 1.0;          // audio thread only
    double releaseCoeff = 0.0002;
    std::atomic<bool> limiting  { false };
    std::atomic<bool> nonFinite { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OutputGuardProcessor)
};
