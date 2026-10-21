//
//  StripTests.cpp — headless tests for the M4 per-node channel strip.
//  Exposes runStripTests(int& checks): appends to the check count, returns failures.
//
//  Covers: NodeStripProcessor DSP (gain + balance law + post-gain metering),
//  GraphDocument gain/pan defaults + round-trip, and a live in->strip->out graph.
//

#include "../Source/Engine/NodeStripProcessor.h"
#include "../Source/Engine/GraphDocument.h"
#include <iostream>
#include <cmath>
#include <limits>

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

    // Fill a stereo buffer with constant values, run one process pass.
    void runStrip (NodeStripProcessor& s, AudioBuffer<float>& buf, float l, float r)
    {
        for (int i = 0; i < buf.getNumSamples(); ++i) { buf.setSample (0, i, l); buf.setSample (1, i, r); }
        MidiBuffer midi;
        s.processBlock (buf, midi);
    }
}

int runStripTests (int& checks)
{
    failures = 0;
    std::cout << "Channel-strip tests" << std::endl;

    // --- 1. Gain scales both channels; meter reflects the post-gain level -----
    {
        NodeStripProcessor s;
        s.setPlayConfigDetails (2, 2, 44100.0, 16);
        s.prepareToPlay (44100.0, 16);
        s.setGain (0.5f);
        s.setPan (0.0f);

        AudioBuffer<float> buf (2, 16);
        runStrip (s, buf, 1.0f, 1.0f);
        check (checks, near (buf.getSample (0, 0), 0.5f), "gain 0.5 halves left");
        check (checks, near (buf.getSample (1, 0), 0.5f), "gain 0.5 halves right");
        check (checks, near (s.getTap().read().peak[0], 0.5f), "strip meter reads post-gain level");
    }

    // --- 2. Balance law: center unity, hard pan mutes the opposite channel ----
    {
        NodeStripProcessor s;
        s.setPlayConfigDetails (2, 2, 44100.0, 16);
        s.prepareToPlay (44100.0, 16);
        AudioBuffer<float> buf (2, 16);

        s.setGain (1.0f); s.setPan (0.0f);
        runStrip (s, buf, 1.0f, 1.0f);
        check (checks, near (buf.getSample (0,0), 1.0f) && near (buf.getSample (1,0), 1.0f), "pan 0 is unity both channels");

        s.setPan (1.0f);   // hard right -> left muted
        runStrip (s, buf, 1.0f, 1.0f);
        check (checks, near (buf.getSample (0,0), 0.0f), "pan +1 mutes left");
        check (checks, near (buf.getSample (1,0), 1.0f), "pan +1 keeps right");

        s.setPan (-1.0f);  // hard left -> right muted
        runStrip (s, buf, 1.0f, 1.0f);
        check (checks, near (buf.getSample (0,0), 1.0f), "pan -1 keeps left");
        check (checks, near (buf.getSample (1,0), 0.0f), "pan -1 mutes right");

        s.setPan (5.0f);   // out of range -> clamped to +1
        check (checks, near (s.getPan(), 1.0f), "pan is clamped to [-1,1]");
    }

    // --- 3. GraphDocument gain/pan defaults + loss-free round-trip ------------
    {
        GraphDocument doc;
        PluginDescription d; d.name = "Fx"; d.pluginFormatName = "VST3";
        const String uid = doc.addPluginNode (d, 0.5f, 0.5f);

        check (checks, near (doc.getGain (uid), 1.0f), "default gain is unity");
        check (checks, near (doc.getPan (uid), 0.0f),  "default pan is centre");

        doc.setGain (uid, 0.25f);
        doc.setPan  (uid, -0.4f);

        GraphDocument rt = GraphDocument::fromXml (doc.toXml());
        check (checks, rt.isValid(), "doc with gain/pan round-trips valid");
        check (checks, near (rt.getGain (uid), 0.25f), "gain survives round-trip");
        check (checks, near (rt.getPan (uid), -0.4f),  "pan survives round-trip");
    }

    // --- 4. Live graph: audioIn -> strip -> audioOut --------------------------
    {
        using IOProc = AudioProcessorGraph::AudioGraphIOProcessor;
        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, 44100.0, 32);
        graph.prepareToPlay (44100.0, 32);

        auto inNode    = graph.addNode (std::make_unique<IOProc> (IOProc::audioInputNode));
        auto outNode   = graph.addNode (std::make_unique<IOProc> (IOProc::audioOutputNode));
        auto stripNode = graph.addNode (std::make_unique<NodeStripProcessor>());
        for (int ch = 0; ch < 2; ++ch)
        {
            graph.addConnection ({ { inNode->nodeID,    ch }, { stripNode->nodeID, ch } });
            graph.addConnection ({ { stripNode->nodeID, ch }, { outNode->nodeID,   ch } });
        }

        auto* strip = dynamic_cast<NodeStripProcessor*> (stripNode->getProcessor());
        check (checks, strip != nullptr, "strip present in graph");
        if (strip != nullptr) strip->setGain (0.5f);

        AudioBuffer<float> block (2, 32);
        for (int i = 0; i < 32; ++i) { block.setSample (0, i, 1.0f); block.setSample (1, i, 1.0f); }
        MidiBuffer midi;
        graph.processBlock (block, midi);

        check (checks, near (block.getSample (0, 0), 0.5f), "in-graph strip applies gain to the signal");
        if (strip != nullptr)
            check (checks, near (strip->getTap().read().peak[0], 0.5f), "in-graph strip meter tracks post-gain level");

        graph.clear();
    }

    // --- Sanitised persisted values + NaN-proof strip -------------------------
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        check (checks, GraphDocument::sanitise (nan, 0.0f, 2.0f, 1.0f) == 1.0f, "sanitise: NaN -> fallback");
        check (checks, GraphDocument::sanitise (inf, 0.0f, 2.0f, 1.0f) == 1.0f, "sanitise: Inf -> fallback");
        check (checks, GraphDocument::sanitise (1000.0f, 0.0f, 2.0f, 1.0f) == 2.0f, "sanitise: clamps high");

        GraphDocument doc;
        const String uid = doc.getIoNodeUid (true);
        doc.getNodeByUid (uid).setProperty ("gain", 1000.0f, nullptr);   // as a corrupt file would
        check (checks, doc.getGain (uid) == 2.0f, "document: gain 1000 reads back as 2");
        doc.getNodeByUid (uid).setProperty ("gain", nan, nullptr);
        check (checks, doc.getGain (uid) == 1.0f, "document: NaN gain reads back as 1");
        doc.getNodeByUid (uid).setProperty ("pan", -5.0f, nullptr);
        check (checks, doc.getPan (uid) == -1.0f, "document: pan -5 reads back as -1");
        doc.getNodeByUid (uid).setProperty ("mix", nan, nullptr);
        check (checks, doc.getMix (uid) == 1.0f, "document: NaN mix reads back as 1");
        doc.setGain (uid, 1000.0f);
        check (checks, doc.getGain (uid) == 2.0f, "document: setGain clamps");
        doc.getNodeByUid (uid).setProperty ("x", 7.0f, nullptr);
        check (checks, doc.getNodeX (uid) == 1.0f, "document: node x clamped to 0..1");

        NodeStripProcessor s;
        s.setGain (nan);
        check (checks, s.getGain() == 1.0f, "strip: NaN gain setter -> 1");
        AudioBuffer<float> buf (2, 8);
        for (int i = 0; i < 8; ++i) { buf.setSample (0, i, i % 2 ? nan : 0.5f); buf.setSample (1, i, inf); }
        MidiBuffer midi;
        s.processBlock (buf, midi);
        bool finite = true;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 8; ++i) finite = finite && std::isfinite (buf.getSample (ch, i));
        check (checks, finite, "strip: NaN/Inf input yields finite output");
        check (checks, near (buf.getSample (0, 0), 0.5f), "strip: finite samples are untouched");
    }

    std::cout << (failures == 0 ? "  strip ok" : "  strip FAILED") << std::endl;
    return failures;
}
