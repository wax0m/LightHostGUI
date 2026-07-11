//
//  MixTests.cpp — headless tests for the per-node dry/wet mix (MixProcessor +
//  GraphDocument mix prop). Exposes runMixTests(int& checks): appends to the
//  caller's check count and returns the failure count.
//
//  Covers: MixProcessor DSP math + clamping, GraphDocument mix default/clamp/
//  round-trip, and a live 4-in/2-out graph that blends a known wet and dry
//  source (proves the parallel-branch channel wiring the controller relies on).
//

#include "../Source/Engine/MixProcessor.h"
#include "../Source/Engine/GraphDocument.h"
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

    // Fill a 4-channel buffer: wet on 0,1 and dry on 2,3, then one process pass.
    void runMix (MixProcessor& m, AudioBuffer<float>& buf, float wet, float dry)
    {
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            buf.setSample (0, i, wet); buf.setSample (1, i, wet);
            buf.setSample (2, i, dry); buf.setSample (3, i, dry);
        }
        MidiBuffer midi;
        m.processBlock (buf, midi);
    }

    // Stereo processor that writes a constant to both output channels (a signal
    // source for the graph-level test). Ignores its input.
    class ConstProcessor : public AudioProcessor
    {
    public:
        explicit ConstProcessor (float v)
            : AudioProcessor (BusesProperties()
                  .withInput  ("In",  AudioChannelSet::stereo(), true)
                  .withOutput ("Out", AudioChannelSet::stereo(), true)),
              value (v) {}
        const String getName() const override { return "Const"; }
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}
        void processBlock (AudioBuffer<float>& b, MidiBuffer&) override
        {
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                for (int i = 0; i < b.getNumSamples(); ++i)
                    b.setSample (ch, i, value);
        }
        using AudioProcessor::processBlock;
        AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        double getTailLengthSeconds() const override { return 0.0; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const String getProgramName (int) override { return {}; }
        void changeProgramName (int, const String&) override {}
        void getStateInformation (MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}
    private:
        float value;
    };
}

int runMixTests (int& checks)
{
    failures = 0;
    std::cout << "Dry/wet mix tests" << std::endl;

    // --- 1. MixProcessor DSP: out = (1-mix)*dry + mix*wet ---------------------
    {
        MixProcessor m;
        m.setPlayConfigDetails (4, 2, 44100.0, 16);
        m.prepareToPlay (44100.0, 16);
        AudioBuffer<float> buf (4, 16);

        m.setMix (1.0f);  runMix (m, buf, 2.0f, 1.0f);   // fully wet
        check (checks, near (buf.getSample (0, 0), 2.0f) && near (buf.getSample (1, 0), 2.0f),
               "mix 1.0 passes the wet signal");

        m.setMix (0.0f);  runMix (m, buf, 2.0f, 1.0f);   // fully dry
        check (checks, near (buf.getSample (0, 0), 1.0f) && near (buf.getSample (1, 0), 1.0f),
               "mix 0.0 passes the dry signal");

        m.setMix (0.5f);  runMix (m, buf, 2.0f, 1.0f);   // half + half
        check (checks, near (buf.getSample (0, 0), 1.5f) && near (buf.getSample (1, 0), 1.5f),
               "mix 0.5 is the linear blend");

        m.setMix (0.25f); runMix (m, buf, 4.0f, 0.0f);   // asymmetric: 0.25*4 + 0.75*0
        check (checks, near (buf.getSample (0, 0), 1.0f), "mix weights wet/dry independently");
    }

    // --- 2. Mix is clamped to [0,1] ------------------------------------------
    {
        MixProcessor m;
        m.setMix (2.0f);  check (checks, near (m.getMix(), 1.0f), "mix clamps above 1");
        m.setMix (-1.0f); check (checks, near (m.getMix(), 0.0f), "mix clamps below 0");
    }

    // --- 3. GraphDocument mix default + clamp + round-trip -------------------
    {
        GraphDocument doc;
        PluginDescription d; d.name = "Fx"; d.pluginFormatName = "VST3";
        const String uid = doc.addPluginNode (d, 0.5f, 0.5f);

        check (checks, near (doc.getMix (uid), 1.0f), "default mix is fully wet");

        doc.setMix (uid, 1.7f);
        check (checks, near (doc.getMix (uid), 1.0f), "doc clamps mix above 1");
        doc.setMix (uid, 0.3f);

        GraphDocument rt = GraphDocument::fromXml (doc.toXml());
        check (checks, rt.isValid(), "doc with mix round-trips valid");
        check (checks, near (rt.getMix (uid), 0.3f), "mix survives round-trip");
    }

    // --- 4. Live graph: constWet + constDry -> mix(4-in/2-out) -> audioOut ----
    {
        using IOProc = AudioProcessorGraph::AudioGraphIOProcessor;
        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, 44100.0, 32);
        graph.prepareToPlay (44100.0, 32);

        auto wetNode = graph.addNode (std::make_unique<ConstProcessor> (2.0f));
        auto dryNode = graph.addNode (std::make_unique<ConstProcessor> (1.0f));
        auto mixNode = graph.addNode (std::make_unique<MixProcessor>());
        auto outNode = graph.addNode (std::make_unique<IOProc> (IOProc::audioOutputNode));

        for (int ch = 0; ch < 2; ++ch)
        {
            graph.addConnection ({ { wetNode->nodeID, ch }, { mixNode->nodeID, ch } });       // wet -> 0,1
            graph.addConnection ({ { dryNode->nodeID, ch }, { mixNode->nodeID, ch + 2 } });    // dry -> 2,3
            graph.addConnection ({ { mixNode->nodeID, ch }, { outNode->nodeID, ch } });
        }

        auto* mix = dynamic_cast<MixProcessor*> (mixNode->getProcessor());
        check (checks, mix != nullptr, "mix node present in graph");
        if (mix != nullptr) mix->setMix (0.5f);

        AudioBuffer<float> block (2, 32);
        block.clear();
        MidiBuffer midi;
        graph.processBlock (block, midi);

        check (checks, near (block.getSample (0, 0), 1.5f) && near (block.getSample (1, 0), 1.5f),
               "in-graph mix blends the parallel wet/dry branches (0.5*2 + 0.5*1)");

        if (mix != nullptr) mix->setMix (0.0f);
        block.clear();
        graph.processBlock (block, midi);
        check (checks, near (block.getSample (0, 0), 1.0f), "in-graph mix 0 = dry branch only");

        graph.clear();
    }

    std::cout << (failures == 0 ? "  mix ok" : "  mix FAILED") << std::endl;
    return failures;
}
