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
    // The layout is built explicitly and applied only if the plugin approves it via
    // checkBusesLayoutSupported, then set with setBusesLayout. JUCE's coarse
    // setPlayConfigDetails(2, 2, …) is avoided on purpose: it first validates the
    // plugin's CURRENT layout, which some plugins reject (Debug assert), and it touches
    // a main input bus even when the plugin has none.
    //   A: main buses stereo, every other bus disabled (the old setPlayConfigDetails result);
    //   B: main buses stereo, other buses as they are (for plugins that need e.g. their
    //      side-chain enabled).
    // If neither is supported the plugin is left untouched (genuinely mono-only
    // processors). Returns true if every main bus the plugin has ends up stereo, and it
    // has at least one.
    inline bool forceStereo (juce::AudioProcessor& p, double sampleRate, int blockSize)
    {
        using juce::AudioChannelSet;

        p.setRateAndBufferSizeDetails (sampleRate, blockSize);

        const bool hasIn  = p.getBusCount (true)  > 0;
        const bool hasOut = p.getBusCount (false) > 0;

        auto wanted = p.getBusesLayout();
        if (hasIn)  wanted.getChannelSet (true,  0) = AudioChannelSet::stereo();
        if (hasOut) wanted.getChannelSet (false, 0) = AudioChannelSet::stereo();

        auto disableSide = wanted;
        for (int i = 1; i < disableSide.inputBuses.size();  ++i) disableSide.inputBuses.getReference (i)  = AudioChannelSet::disabled();
        for (int i = 1; i < disableSide.outputBuses.size(); ++i) disableSide.outputBuses.getReference (i) = AudioChannelSet::disabled();

        if (p.getBusesLayout() != disableSide)
        {
            for (const auto* candidate : { &disableSide, &wanted })
                if (p.checkBusesLayoutSupported (*candidate) && p.setBusesLayout (*candidate))
                    break;
        }

        const auto now = p.getBusesLayout();
        return (hasIn || hasOut)
            && (! hasIn  || now.getMainInputChannelSet()  == AudioChannelSet::stereo())
            && (! hasOut || now.getMainOutputChannelSet() == AudioChannelSet::stereo());
    }
}
