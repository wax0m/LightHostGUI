//
//  PluginParameters.h — a thin, testable bridge over a hosted plugin's
//  parameters. A host reads the live AudioPluginInstance's parameter list
//  (juce::AudioProcessor::getParameters()) — this is NOT AudioProcessorValueTreeState
//  (that is for authoring your own plugin). Free functions so they can be unit
//  tested against any synthetic AudioProcessor without a real plugin or device.
//
//  Threading: call on the message thread. setValue uses setValueNotifyingHost,
//  the same path JUCE's own AudioPluginHost uses to drive a hosted plugin.
//

#pragma once

#include <JuceHeader.h>
#include <vector>

namespace lighthost::params
{
    struct ParamInfo
    {
        int          index = 0;      // position in getParameters()
        juce::String name;           // human-readable parameter name
        juce::String label;          // unit suffix (e.g. "Hz", "dB"), may be empty
        float        value = 0.0f;   // current normalized value, 0..1
        float        defaultValue = 0.0f;  // plugin's normalized default (for reset-to-default)
        juce::String text;           // current value formatted by the plugin
        bool         automatable = true;
        bool         boolean = false;
        int          numSteps = 0;   // 0 => continuous
    };

    // Snapshot a processor's parameters. automatableOnly filters out meta/hidden
    // params (bypass, program, etc.) that plugins mark non-automatable.
    inline std::vector<ParamInfo> collect (juce::AudioProcessor& proc, bool automatableOnly = false)
    {
        std::vector<ParamInfo> out;
        const auto& params = proc.getParameters();
        for (int i = 0; i < params.size(); ++i)
        {
            auto* p = params.getUnchecked (i);
            if (automatableOnly && ! p->isAutomatable())
                continue;

            ParamInfo info;
            info.index       = i;
            info.name        = p->getName (128);
            info.label       = p->getLabel();
            info.value       = p->getValue();
            info.defaultValue = p->getDefaultValue();
            info.text        = p->getCurrentValueAsText();
            info.automatable = p->isAutomatable();
            info.boolean     = p->isBoolean();
            info.numSteps    = p->getNumSteps();
            out.push_back (std::move (info));
        }
        return out;
    }

    // The first `count` parameters the card should surface (automatable first).
    inline std::vector<ParamInfo> curate (juce::AudioProcessor& proc, int count)
    {
        auto all = collect (proc, /*automatableOnly*/ true);
        if (all.empty())
            all = collect (proc, false);          // fall back if none are automatable
        if ((int) all.size() > count)
            all.resize ((size_t) count);
        return all;
    }

    inline bool setValue (juce::AudioProcessor& proc, int index, float normalized)
    {
        const auto& params = proc.getParameters();
        if (index < 0 || index >= params.size())
            return false;
        params.getUnchecked (index)->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, normalized));
        return true;
    }

    inline float getValue (juce::AudioProcessor& proc, int index)
    {
        const auto& params = proc.getParameters();
        if (index < 0 || index >= params.size())
            return 0.0f;
        return params.getUnchecked (index)->getValue();
    }

    // The plugin's own formatted text for parameter `index` (its getCurrentValueAsText).
    // Used to update a card knob's readout live while the user drags it.
    inline juce::String valueText (juce::AudioProcessor& proc, int index)
    {
        const auto& params = proc.getParameters();
        if (index < 0 || index >= params.size())
            return {};
        return params.getUnchecked (index)->getCurrentValueAsText();
    }
}
