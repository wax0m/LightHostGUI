//
//  MeterTap.h — lock-free peak/RMS meter shared between the audio thread and GUI.
//
//  Single-writer (audio thread, measureBlock) / single-reader (message thread,
//  read) by design. Values are plain std::atomic<float> with relaxed ordering:
//  a meter can tolerate reading a value one block stale, and we never need the
//  peak and rms of a channel to be mutually consistent to a sample. No locks, no
//  allocation, no blocking on the audio thread — priority #1 is audio stability.
//

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cmath>

class MeterTap
{
public:
    static constexpr int numChannels = 2;

    struct Levels
    {
        float peak[numChannels] { 0.0f, 0.0f };
        float rms [numChannels] { 0.0f, 0.0f };
    };

    MeterTap() noexcept
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            peak[ch].store (0.0f, std::memory_order_relaxed);
            rms [ch].store (0.0f, std::memory_order_relaxed);
        }
    }

    // Audio thread: measure channels 0..numChannels-1 of a block. Never allocates.
    void measureBlock (const juce::AudioBuffer<float>& buffer) noexcept
    {
        const int numSamples = buffer.getNumSamples();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (ch >= buffer.getNumChannels() || numSamples <= 0)
            {
                peak[ch].store (0.0f, std::memory_order_relaxed);
                rms [ch].store (0.0f, std::memory_order_relaxed);
                continue;
            }

            const float* d = buffer.getReadPointer (ch);
            float pk = 0.0f;
            float sumSq = 0.0f;

            for (int i = 0; i < numSamples; ++i)
            {
                const float s = d[i];
                const float a = std::abs (s);
                if (a > pk) pk = a;
                sumSq += s * s;
            }

            peak[ch].store (pk, std::memory_order_relaxed);
            rms [ch].store (std::sqrt (sumSq / (float) numSamples), std::memory_order_relaxed);
        }
    }

    // Message thread: snapshot the current levels for painting.
    Levels read() const noexcept
    {
        Levels l;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            l.peak[ch] = peak[ch].load (std::memory_order_relaxed);
            l.rms [ch] = rms [ch].load (std::memory_order_relaxed);
        }
        return l;
    }

    static float toDecibels (float linear) noexcept
    {
        return juce::Decibels::gainToDecibels (linear);
    }

private:
    std::atomic<float> peak[numChannels];
    std::atomic<float> rms [numChannels];

    JUCE_DECLARE_NON_COPYABLE (MeterTap)
};
