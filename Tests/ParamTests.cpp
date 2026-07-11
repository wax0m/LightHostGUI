//
//  ParamTests.cpp — headless tests for the M5 plugin-parameter bridge.
//  Exposes runParamTests(int& checks). Uses a synthetic AudioProcessor with
//  known parameters, so it needs no real plugin or audio device.
//

#include "../Source/Engine/PluginParameters.h"
#include <iostream>
#include <cmath>

using namespace juce;

namespace
{
    int failures = 0;
    void check (int& checks, bool cond, const String& msg)
    {
        ++checks;
        if (! cond) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; }
    }
    bool near (float a, float b, float tol = 1.0e-3f) { return std::abs (a - b) <= tol; }

    // Minimal processor exposing three parameters of different kinds.
    struct TestProc : juce::AudioProcessor
    {
        TestProc()
        {
            addParameter (freq = new juce::AudioParameterFloat ({ "freq", 1 }, "Freq",
                              juce::NormalisableRange<float> (20.0f, 20000.0f), 1000.0f,
                              juce::AudioParameterFloatAttributes().withLabel ("Hz")));
            addParameter (gain = new juce::AudioParameterFloat ({ "gain", 1 }, "Gain", -15.0f, 15.0f, 0.0f));
            addParameter (onOff = new juce::AudioParameterBool  ({ "on",   1 }, "On", true));
        }
        const String getName() const override { return "TestProc"; }
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}
        void processBlock (AudioBuffer<float>&, MidiBuffer&) override {}
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const String getProgramName (int) override { return {}; }
        void changeProgramName (int, const String&) override {}
        void getStateInformation (MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}

        juce::AudioParameterFloat* freq = nullptr;
        juce::AudioParameterFloat* gain = nullptr;
        juce::AudioParameterBool*  onOff = nullptr;
    };
}

int runParamTests (int& checks)
{
    failures = 0;
    std::cout << "Plugin-parameter bridge tests" << std::endl;

    TestProc proc;

    // --- collect: names, order, kinds, units --------------------------------
    auto all = lighthost::params::collect (proc);
    check (checks, all.size() == 3, "collect returns all three parameters");
    if (all.size() == 3)
    {
        check (checks, all[0].name == "Freq" && all[1].name == "Gain" && all[2].name == "On",
               "parameter names and order preserved");
        check (checks, all[0].label == "Hz", "unit label captured (Hz)");
        check (checks, all[2].boolean, "bool parameter flagged boolean");
        check (checks, near (all[1].value, 0.5f), "Gain default (0 dB in -15..15) is normalized 0.5");
        check (checks, all[0].value > 0.0f && all[0].value < 0.1f, "Freq default (1 kHz) sits low in a 20..20k range");

        // defaultValue: the plugin's normalized default, surfaced for double-click reset.
        check (checks, near (all[1].defaultValue, 0.5f), "Gain defaultValue is normalized 0.5 (0 dB)");
        check (checks, near (all[2].defaultValue, 1.0f), "bool parameter defaultValue is 1.0 (on)");
    }

    // --- setValue drives the parameter, getValue reflects it ----------------
    check (checks, lighthost::params::setValue (proc, 1, 0.0f), "setValue accepts a valid index");
    check (checks, near (lighthost::params::getValue (proc, 1), 0.0f), "Gain now reads 0.0 (=-15 dB)");
    check (checks, near ((float) *proc.gain, -15.0f, 0.01f), "underlying AudioParameterFloat moved to -15 dB");

    lighthost::params::setValue (proc, 1, 1.0f);
    check (checks, near ((float) *proc.gain, 15.0f, 0.01f), "Gain drives to +15 dB at normalized 1.0");

    // --- bounds + clamping ---------------------------------------------------
    check (checks, ! lighthost::params::setValue (proc, 7, 0.5f), "out-of-range index is rejected");
    lighthost::params::setValue (proc, 0, 5.0f);   // over-range value
    check (checks, lighthost::params::getValue (proc, 0) <= 1.0f, "over-range normalized value is clamped to <=1");

    // --- curate: cap the count for the card ---------------------------------
    auto two = lighthost::params::curate (proc, 2);
    check (checks, two.size() == 2, "curate caps the parameter count for the card");

    std::cout << (failures == 0 ? "  params ok" : "  params FAILED") << std::endl;
    return failures;
}
