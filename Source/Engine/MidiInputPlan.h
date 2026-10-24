//
//  MidiInputPlan.h — which MIDI inputs to open and close when the device list changes (hot-plug).
//  Pure and headless-testable; IconMenu applies the plan to the AudioDeviceManager.
//

#pragma once

#include <JuceHeader.h>

namespace lighthost::midi
{
    // The two fields of juce::MidiDeviceInfo (kept apart so this header needs no audio-devices module).
    struct Device
    {
        juce::String name, identifier;
    };

    struct Plan
    {
        juce::Array<Device> toEnable;   // inputs that are present and not open
        juce::StringArray toDisable;                  // inputs that were present and are gone: close the stale handle
    };

    // available:         the inputs the system lists now
    // previousIds:       identifiers of the inputs listed the last time (empty at start)
    // enabledIds:        identifiers that are open in the device manager now
    // A device that went away is closed, so that the same device plugged in again is opened afresh instead of
    // being mistaken for the dead handle that is still marked as open.
    inline Plan planMidiInputs (const juce::Array<Device>& available,
                                const juce::StringArray& previousIds,
                                const juce::StringArray& enabledIds)
    {
        Plan plan;

        juce::StringArray availableIds;
        for (const auto& d : available)
            availableIds.add (d.identifier);

        for (const auto& id : previousIds)
            if (! availableIds.contains (id) && enabledIds.contains (id))
                plan.toDisable.add (id);

        for (const auto& d : available)
            if (! enabledIds.contains (d.identifier) || plan.toDisable.contains (d.identifier))
                plan.toEnable.add (d);

        return plan;
    }

    // "MIDI input Kawai USB MIDI is in use by another app" (names joined); {} when none.
    inline juce::String midiInUseNotice (const juce::StringArray& names)
    {
        if (names.isEmpty())
            return {};

        return "MIDI input " + names.joinIntoString (", ") + (names.size() == 1 ? " is" : " are") + " in use by another app";
    }
}
