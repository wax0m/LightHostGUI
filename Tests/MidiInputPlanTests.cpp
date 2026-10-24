//
//  MidiInputPlanTests.cpp — headless tests for the MIDI hot-plug plan (MidiInputPlan.h): which inputs to open
//  and which stale ones to close when the device list changes. Exposes runMidiInputPlanTests(int& checks).
//

#include "../Source/Engine/MidiInputPlan.h"
#include <iostream>

using namespace juce;
namespace mi = lighthost::midi;

namespace
{
    int failures = 0;
    void check (int& checks, bool cond, const String& msg)
    {
        ++checks;
        if (! cond) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; }
    }

    Array<mi::Device> devices (std::initializer_list<const char*> ids)
    {
        Array<mi::Device> out;
        for (auto id : ids)
            out.add ({ String ("Name of ") + id, id });
        return out;
    }

    String idsOf (const Array<mi::Device>& a)
    {
        StringArray s;
        for (const auto& d : a)
            s.add (d.identifier);
        return s.joinIntoString (",");
    }
}

int runMidiInputPlanTests (int& checks)
{
    failures = 0;
    std::cout << "MIDI input plan tests" << std::endl;

    // Start: everything present is opened, nothing is closed.
    {
        const auto plan = mi::planMidiInputs (devices ({ "a", "b" }), {}, {});
        check (checks, idsOf (plan.toEnable) == "a,b" && plan.toDisable.isEmpty(), "start: all present inputs are opened");
    }

    // A piano switched on later: only the new input is opened.
    {
        const auto plan = mi::planMidiInputs (devices ({ "a", "piano" }), { "a" }, { "a" });
        check (checks, idsOf (plan.toEnable) == "piano" && plan.toDisable.isEmpty(), "hot-plug: only the new input is opened");
    }

    // Nothing changed: nothing to do (the callback also fires for unrelated list changes).
    {
        const auto plan = mi::planMidiInputs (devices ({ "a", "b" }), { "a", "b" }, { "a", "b" });
        check (checks, plan.toEnable.isEmpty() && plan.toDisable.isEmpty(), "no change: nothing to do");
    }

    // Unplugged: its handle is closed; plugged in again: it is opened afresh.
    {
        const auto gone = mi::planMidiInputs (devices ({ "a" }), { "a", "piano" }, { "a", "piano" });
        check (checks, gone.toDisable == StringArray ("piano") && gone.toEnable.isEmpty(), "unplug: the stale handle is closed");

        const auto back = mi::planMidiInputs (devices ({ "a", "piano" }), { "a" }, { "a" });
        check (checks, idsOf (back.toEnable) == "piano", "replug: the input is opened again");
    }

    // An input the user (or the saved state) did not leave open is opened; one that failed to open before is retried.
    {
        const auto plan = mi::planMidiInputs (devices ({ "a", "busy" }), { "a", "busy" }, { "a" });
        check (checks, idsOf (plan.toEnable) == "busy", "retry: an input that is present but not open is tried again");
    }

    // A gone device that was never open is not closed.
    {
        const auto plan = mi::planMidiInputs (devices ({ "a" }), { "a", "busy" }, { "a" });
        check (checks, plan.toDisable.isEmpty(), "gone and never open: nothing to close");
    }

    // The notice.
    {
        check (checks, mi::midiInUseNotice ({}).isEmpty(), "notice: none for an empty list");
        check (checks, mi::midiInUseNotice ({ "Kawai USB MIDI" }) == "MIDI input Kawai USB MIDI is in use by another app", "notice: one input");
        check (checks, mi::midiInUseNotice ({ "A", "B" }) == "MIDI input A, B are in use by another app", "notice: several inputs");
    }

    return failures;
}
