//
//  MeterTests.cpp — headless tests for the M3 metering engine.
//
//  Exposes runMeterTests(int& checks): appends to the caller's check count and
//  returns the number of failures, so it folds into the LightHostTests summary.
//
//  Covers three layers with no audio device and no plugins:
//    1. MeterTap DSP — peak/RMS of known buffers;
//    2. MeterProcessor — measures AND leaves the buffer untouched (pass-through);
//    3. a live AudioProcessorGraph (audioIn -> meter -> audioOut) — proves the
//       meter measures real signal in-graph and is transparent.
//

#include "../Source/Engine/MeterProcessor.h"
#include "../Source/Engine/PassThroughProcessor.h"
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

    bool near (float a, float b, float tol = 1.0e-4f) { return std::abs (a - b) <= tol; }
}

int runMeterTests (int& checks)
{
    failures = 0;
    std::cout << "Meter engine tests" << std::endl;

    // --- 1. MeterTap DSP ---------------------------------------------------
    {
        MeterTap tap;
        AudioBuffer<float> buf (2, 8);
        buf.clear();
        // ch0: full-scale square -> peak 1, rms 1. ch1: constant 0.5 -> peak/rms 0.5.
        for (int i = 0; i < 8; ++i) { buf.setSample (0, i, (i % 2 == 0) ? 1.0f : -1.0f); buf.setSample (1, i, 0.5f); }
        tap.measureBlock (buf);
        const auto lv = tap.read();
        check (checks, near (lv.peak[0], 1.0f), "tap ch0 peak == 1");
        check (checks, near (lv.rms [0], 1.0f), "tap ch0 rms == 1");
        check (checks, near (lv.peak[1], 0.5f), "tap ch1 peak == 0.5");
        check (checks, near (lv.rms [1], 0.5f), "tap ch1 rms == 0.5");

        AudioBuffer<float> silence (2, 8); silence.clear();
        tap.measureBlock (silence);
        const auto lv2 = tap.read();
        check (checks, near (lv2.peak[0], 0.0f) && near (lv2.rms[0], 0.0f), "tap reads silence as 0");
    }

    // --- 2. MeterProcessor is a transparent pass-through -------------------
    {
        MeterProcessor meter;
        meter.setPlayConfigDetails (2, 2, 44100.0, 16);
        meter.prepareToPlay (44100.0, 16);

        AudioBuffer<float> buf (2, 16);
        for (int i = 0; i < 16; ++i) { buf.setSample (0, i, 0.25f); buf.setSample (1, i, -0.75f); }
        AudioBuffer<float> copy; copy.makeCopyOf (buf);

        MidiBuffer midi;
        meter.processBlock (buf, midi);

        bool identical = true;
        for (int ch = 0; ch < 2 && identical; ++ch)
            for (int i = 0; i < 16; ++i)
                if (! near (buf.getSample (ch, i), copy.getSample (ch, i))) { identical = false; break; }
        check (checks, identical, "MeterProcessor leaves the buffer unmodified");

        const auto lv = meter.getTap().read();
        check (checks, near (lv.peak[0], 0.25f), "MeterProcessor measured ch0 peak");
        check (checks, near (lv.peak[1], 0.75f), "MeterProcessor measured ch1 peak (abs)");
    }

    // --- 3. Live graph: audioIn -> meter -> audioOut ----------------------
    {
        using IOProc = AudioProcessorGraph::AudioGraphIOProcessor;
        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, 44100.0, 64);
        graph.prepareToPlay (44100.0, 64);

        auto inNode    = graph.addNode (std::make_unique<IOProc> (IOProc::audioInputNode));
        auto outNode   = graph.addNode (std::make_unique<IOProc> (IOProc::audioOutputNode));
        auto meterNode = graph.addNode (std::make_unique<MeterProcessor>());

        for (int ch = 0; ch < 2; ++ch)
        {
            graph.addConnection ({ { inNode->nodeID,    ch }, { meterNode->nodeID, ch } });
            graph.addConnection ({ { meterNode->nodeID, ch }, { outNode->nodeID,   ch } });
        }

        auto* meter = dynamic_cast<MeterProcessor*> (meterNode->getProcessor());
        check (checks, meter != nullptr, "meter processor present in graph");

        // Push a constant 0.5 through the input; expect it to reach the meter and
        // pass through to the output unchanged.
        AudioBuffer<float> block (2, 64);
        for (int i = 0; i < 64; ++i) { block.setSample (0, i, 0.5f); block.setSample (1, i, 0.5f); }
        MidiBuffer midi;
        graph.processBlock (block, midi);

        if (meter != nullptr)
        {
            const auto lv = meter->getTap().read();
            check (checks, lv.peak[0] > 0.4f, "in-graph meter sees non-silent signal");
        }
        check (checks, near (block.getSample (0, 0), 0.5f), "signal passes through the graph unchanged");

        // Silence -> meter should fall back to ~0.
        AudioBuffer<float> quiet (2, 64); quiet.clear();
        graph.processBlock (quiet, midi);
        if (meter != nullptr)
            check (checks, meter->getTap().read().peak[0] < 1.0e-4f, "in-graph meter reads silence as 0");

        graph.clear();
    }

    // --- 4. PassThroughProcessor is transparent in a live graph -------------
    {
        using IOProc = AudioProcessorGraph::AudioGraphIOProcessor;
        AudioProcessorGraph g;
        g.setPlayConfigDetails (2, 2, 44100.0, 64);
        g.prepareToPlay (44100.0, 64);
        auto in  = g.addNode (std::make_unique<IOProc> (IOProc::audioInputNode));
        auto pt  = g.addNode (std::make_unique<PassThroughProcessor>());
        auto out = g.addNode (std::make_unique<IOProc> (IOProc::audioOutputNode));
        for (int ch = 0; ch < 2; ++ch)
        {
            g.addConnection ({ { in->nodeID, ch }, { pt->nodeID,  ch } });
            g.addConnection ({ { pt->nodeID, ch }, { out->nodeID, ch } });
        }
        AudioBuffer<float> block (2, 64);
        for (int i = 0; i < 64; ++i) { block.setSample (0, i, 0.42f); block.setSample (1, i, -0.31f); }
        MidiBuffer midi;
        g.processBlock (block, midi);
        check (checks, near (block.getSample (0, 0), 0.42f) && near (block.getSample (1, 0), -0.31f),
               "missing-plugin pass-through carries audio unchanged (chain not severed)");
        g.clear();
    }

    std::cout << (failures == 0 ? "  meters ok" : "  meters FAILED") << std::endl;
    return failures;
}
