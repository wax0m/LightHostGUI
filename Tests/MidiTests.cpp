//
//  MidiTests.cpp — headless tests for the M7 MIDI routing engine slice.
//  Exposes runMidiTests(int& checks). Proves MIDI flows midiInputNode -> plugin
//  on midiChannelIndex in a real AudioProcessorGraph (synthetic MIDI recorder,
//  no device), plus the per-node receivesMidi document flag + round-trip.
//

#include "../Source/Engine/GraphDocument.h"
#include <JuceHeader.h>
#include <iostream>
#include <atomic>

using namespace juce;

namespace
{
    int failures = 0;
    void check (int& checks, bool cond, const String& msg)
    {
        ++checks;
        if (! cond) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; }
    }

    // Stereo pass-through that accepts MIDI and counts the events it receives.
    struct MidiRecorder : AudioProcessor
    {
        MidiRecorder()
            : AudioProcessor (BusesProperties()
                  .withInput  ("In",  AudioChannelSet::stereo(), true)
                  .withOutput ("Out", AudioChannelSet::stereo(), true)) {}
        const String getName() const override { return "MidiRecorder"; }
        using AudioProcessor::processBlock;   // keep the double overload visible
        bool acceptsMidi() const override { return true; }
        bool producesMidi() const override { return false; }
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}
        void processBlock (AudioBuffer<float>&, MidiBuffer& midi) override
        {
            totalEvents += midi.getNumEvents();
        }
        double getTailLengthSeconds() const override { return 0.0; }
        AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const String getProgramName (int) override { return {}; }
        void changeProgramName (int, const String&) override {}
        void getStateInformation (MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}
        std::atomic<int> totalEvents { 0 };
    };
}

int runMidiTests (int& checks)
{
    failures = 0;
    std::cout << "MIDI routing tests" << std::endl;

    // --- 1. Document receivesMidi flag: default true, settable, round-trips --
    {
        GraphDocument doc;
        PluginDescription d; d.name = "Synth"; d.pluginFormatName = "VST3";
        const String uid = doc.addPluginNode (d, 0.5f, 0.5f);
        check (checks, doc.getReceivesMidi (uid), "receivesMidi defaults to true");
        doc.setReceivesMidi (uid, false);
        check (checks, ! doc.getReceivesMidi (uid), "receivesMidi can be cleared");
        GraphDocument rt = GraphDocument::fromXml (doc.toXml());
        check (checks, ! rt.getReceivesMidi (uid), "receivesMidi survives round-trip");
    }

    // --- 2. Live graph: midiInput -> recorder on midiChannelIndex -----------
    {
        using IOProc = AudioProcessorGraph::AudioGraphIOProcessor;
        AudioProcessorGraph g;
        g.setPlayConfigDetails (2, 2, 44100.0, 256);
        g.prepareToPlay (44100.0, 256);

        auto ain  = g.addNode (std::make_unique<IOProc> (IOProc::audioInputNode));
        auto aout = g.addNode (std::make_unique<IOProc> (IOProc::audioOutputNode));
        auto min  = g.addNode (std::make_unique<IOProc> (IOProc::midiInputNode));
        auto rec  = g.addNode (std::make_unique<MidiRecorder>());

        // Put the recorder in the audio path so it is scheduled, and feed it MIDI.
        for (int ch = 0; ch < 2; ++ch)
        {
            g.addConnection ({ { ain->nodeID, ch }, { rec->nodeID, ch } });
            g.addConnection ({ { rec->nodeID, ch }, { aout->nodeID, ch } });
        }
        const bool midiWired = g.addConnection ({ { min->nodeID, AudioProcessorGraph::midiChannelIndex },
                                                  { rec->nodeID, AudioProcessorGraph::midiChannelIndex } });
        check (checks, midiWired, "midiInput -> plugin connection accepted on midiChannelIndex");

        auto* recorder = dynamic_cast<MidiRecorder*> (rec->getProcessor());
        check (checks, recorder != nullptr, "recorder present");

        AudioBuffer<float> buf (2, 256); buf.clear();

        // Silent MIDI block first: nothing should arrive.
        { MidiBuffer empty; g.processBlock (buf, empty); }
        const int afterSilence = recorder != nullptr ? recorder->totalEvents.load() : -1;
        check (checks, afterSilence == 0, "no MIDI arrives when none is sent");

        // Now send a note-on through the graph's MIDI input.
        MidiBuffer midi;
        midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 0);
        buf.clear();
        g.processBlock (buf, midi);
        check (checks, recorder != nullptr && recorder->totalEvents.load() >= 1,
               "note-on routed midiInput -> plugin through the graph");

        g.clear();
    }

    std::cout << (failures == 0 ? "  midi ok" : "  midi FAILED") << std::endl;
    return failures;
}
