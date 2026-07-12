//
//  PluginBuses.h — bus-layout helpers for hosted plugins. A thin, testable wrapper
//  over juce::AudioProcessor bus configuration so the call site (GraphController)
//  and unit tests share one definition.
//

#pragma once

#include <JuceHeader.h>

namespace lighthost::buses
{
    // Force a hosted plugin's MAIN in/out buses to stereo when it supports it, so a
    // plugin that instantiates mono by default (e.g. MeldaProduction MAutoPitch)
    // doesn't collapse a channel in the stereo chain: the host always wires the
    // stereo pair (ch0 + ch1), and a mono plugin only exposes channel 0, leaving the
    // other side silent downstream.
    //
    // setPlayConfigDetails(2, 2, …) sets the main buses to the canonical 2-channel
    // (stereo) set if the plugin's layout supports it, and is a harmless no-op when
    // the plugin is already stereo. Returns true if the plugin ends up 2-in/2-out
    // (false for genuinely mono-only processors, which are left untouched).
    inline bool forceStereo (juce::AudioProcessor& p, double sampleRate, int blockSize)
    {
        if (p.getTotalNumInputChannels() != 2 || p.getTotalNumOutputChannels() != 2)
            p.setPlayConfigDetails (2, 2, sampleRate, blockSize);

        return p.getTotalNumInputChannels() == 2 && p.getTotalNumOutputChannels() == 2;
    }
}
