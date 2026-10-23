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
#include "../Source/Engine/MonoInputProcessor.h"
#include "../Source/Engine/OutputGuardProcessor.h"
#include "../Source/Engine/PluginBuses.h"
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

    // Synthetic processor that instantiates MONO but optionally supports stereo, to
    // exercise lighthost::buses::forceStereo without needing a real plugin.
    struct ConfigurableProc : juce::AudioProcessor
    {
        explicit ConfigurableProc (bool supportsStereoIn)
            : juce::AudioProcessor (BusesProperties()
                  .withInput  ("In",  AudioChannelSet::mono(), true)
                  .withOutput ("Out", AudioChannelSet::mono(), true)),
              supportsStereo (supportsStereoIn) {}

        bool supportsStereo;

        bool isBusesLayoutSupported (const BusesLayout& l) const override
        {
            const auto in  = l.getMainInputChannelSet();
            const auto out = l.getMainOutputChannelSet();
            if (in != out) return false;
            if (in == AudioChannelSet::mono())   return true;
            if (in == AudioChannelSet::stereo()) return supportsStereo;
            return false;
        }

        const String getName() const override { return "Cfg"; }
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
    };

    // Configurable bus layouts for the forceStereo edge cases: an optional mono side-chain
    // input (disable-able or required), no input bus at all (instrument-like), and
    // a plugin that rejects its own initial layout (as Bertom Denoiser does in JUCE's
    // current-layout check).
    struct BusProc : juce::AudioProcessor
    {
        enum class Side { none, optional, required };

        static BusesProperties make (bool withInput, Side side)
        {
            BusesProperties b;
            if (withInput) b = b.withInput ("In", AudioChannelSet::mono(), true);
            if (side != Side::none) b = b.withInput ("SC", AudioChannelSet::mono(), true);
            return b.withOutput ("Out", AudioChannelSet::mono(), true);
        }

        BusProc (bool withInput, Side s) : juce::AudioProcessor (make (withInput, s)), hasInput (withInput), side (s) {}

        bool hasInput; Side side;
        mutable int rejected = 0;   // layouts offered that the plugin refused

        bool isBusesLayoutSupported (const BusesLayout& l) const override
        {
            const auto out = l.getMainOutputChannelSet();
            bool ok = (out == AudioChannelSet::stereo() || out == AudioChannelSet::mono());
            if (hasInput && l.getMainInputChannelSet() != out) ok = false;
            if (side != Side::none)
            {
                const bool scOn = ! l.getChannelSet (true, hasInput ? 1 : 0).isDisabled();
                if (side == Side::required && ! scOn) ok = false;
            }
            if (! ok) ++rejected;
            return ok;
        }

        const String getName() const override { return "Bus"; }
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
    };
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

    // --- 5. forceStereo promotes mono-default plugins that support stereo ----
    {
        ConfigurableProc stereoCapable (true);
        check (checks, stereoCapable.getTotalNumInputChannels() == 1, "synthetic plugin instantiates mono");
        const bool ok = lighthost::buses::forceStereo (stereoCapable, 44100.0, 64);
        check (checks, ok && stereoCapable.getTotalNumInputChannels() == 2
                          && stereoCapable.getTotalNumOutputChannels() == 2,
               "forceStereo promotes a stereo-capable mono-default plugin to 2-in/2-out");

        ConfigurableProc monoOnly (false);
        const bool ok2 = lighthost::buses::forceStereo (monoOnly, 44100.0, 64);
        check (checks, ! ok2 && monoOnly.getTotalNumInputChannels() == 1,
               "forceStereo leaves a mono-only plugin untouched");

        // Optional side-chain: disabled, main stereo (candidate A).
        BusProc optSc (true, BusProc::Side::optional);
        check (checks, lighthost::buses::forceStereo (optSc, 44100.0, 64)
                          && optSc.getBus (true, 0)->getCurrentLayout() == AudioChannelSet::stereo()
                          && optSc.getBus (false, 0)->getCurrentLayout() == AudioChannelSet::stereo()
                          && ! optSc.getBus (true, 1)->isEnabled(),
               "forceStereo: optional side-chain is disabled, main buses stereo");

        // Required side-chain: candidate A refused, B keeps it enabled.
        BusProc reqSc (true, BusProc::Side::required);
        check (checks, lighthost::buses::forceStereo (reqSc, 44100.0, 64)
                          && reqSc.getBus (true, 0)->getCurrentLayout() == AudioChannelSet::stereo()
                          && reqSc.getBus (true, 1)->isEnabled(),
               "forceStereo: required side-chain stays enabled, main buses stereo");

        // Instrument-like: no input bus at all. Output goes stereo, nothing crashes.
        BusProc inst (false, BusProc::Side::none);
        check (checks, inst.getBusCount (true) == 0 && lighthost::buses::forceStereo (inst, 44100.0, 64)
                          && inst.getBus (false, 0)->getCurrentLayout() == AudioChannelSet::stereo(),
               "forceStereo: no input bus, mono output promoted to stereo");

        // Rejects its own initial (mono) layout: still ends up stereo.
        struct PickyProc : BusProc
        {
            PickyProc() : BusProc (true, Side::none) {}
            bool isBusesLayoutSupported (const BusesLayout& l) const override
            {
                return l.getMainInputChannelSet() == AudioChannelSet::stereo()
                    && l.getMainOutputChannelSet() == AudioChannelSet::stereo();
            }
        } picky;
        check (checks, lighthost::buses::forceStereo (picky, 44100.0, 64)
                          && picky.getTotalNumInputChannels() == 2 && picky.getTotalNumOutputChannels() == 2,
               "forceStereo: plugin that rejects its own initial layout ends stereo");

        // Already stereo is a no-op.
        check (checks, lighthost::buses::forceStereo (picky, 44100.0, 64),
               "forceStereo: already-stereo plugin stays stereo");
    }

    // --- 6. MonoInputProcessor sums L+R onto both channels ------------------
    {
        MonoInputProcessor mono;
        AudioBuffer<float> buf (2, 4);
        MidiBuffer m;

        // Right-only input (a mic on the right channel): both channels get it.
        for (int i = 0; i < 4; ++i) { buf.setSample (0, i, 0.0f); buf.setSample (1, i, 0.5f); }
        mono.processBlock (buf, m);
        check (checks, near (buf.getSample (0, 0), 0.5f) && near (buf.getSample (1, 0), 0.5f),
               "mono: right-only input appears on both channels at full level");

        // Distinct L/R fold to the sum on both channels.
        for (int i = 0; i < 4; ++i) { buf.setSample (0, i, 0.4f); buf.setSample (1, i, -0.1f); }
        mono.processBlock (buf, m);
        check (checks, near (buf.getSample (0, 0), 0.3f) && near (buf.getSample (1, 0), 0.3f),
               "mono: both channels become the L+R sum");
    }

    // --- 7. OutputGuardProcessor: zero-latency soft limiter + NaN scrubber --
    {
        constexpr double sr = 48000.0;
        constexpr int blk = 480;
        const float ceiling = OutputGuardProcessor::getCeiling();
        MidiBuffer m;

        const auto fillSine = [] (AudioBuffer<float>& b, float amp, int& phase)
        {
            for (int i = 0; i < b.getNumSamples(); ++i, ++phase)
            {
                const float v = amp * std::sin (MathConstants<float>::twoPi * 1000.0f * (float) phase / 48000.0f);
                b.setSample (0, i, v);
                b.setSample (1, i, v);
            }
        };

        // Below the ceiling: bit-exact.
        {
            OutputGuardProcessor g; g.setRateAndBufferSizeDetails (sr, blk); g.prepareToPlay (sr, blk);
            AudioBuffer<float> in (2, blk), out (2, blk);
            int ph = 0;
            fillSine (in, Decibels::decibelsToGain (-6.0f), ph);
            out.makeCopyOf (in);
            g.processBlock (out, m);
            bool exact = true;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blk; ++i)
                    exact = exact && out.getSample (ch, i) == in.getSample (ch, i);
            check (checks, exact, "guard: -6 dBFS sine passes bit-exact");
            check (checks, ! g.consumeLimiting() && ! g.consumeNonFinite(), "guard: quiet signal sets no latch");
        }

        // +6 dBFS is held to the ceiling; gain recovers; quiet audio is bit-exact again.
        {
            OutputGuardProcessor g; g.setRateAndBufferSizeDetails (sr, blk); g.prepareToPlay (sr, blk);
            AudioBuffer<float> b (2, blk);
            int ph = 0;
            float worst = 0.0f;
            for (int k = 0; k < 10; ++k)
            {
                fillSine (b, Decibels::decibelsToGain (6.0f), ph);
                g.processBlock (b, m);
                worst = jmax (worst, b.getMagnitude (0, blk));
            }
            check (checks, worst <= ceiling + 1.0e-6f, "guard: +6 dBFS sine peaks at or below the ceiling");
            check (checks, worst > ceiling * 0.9f, "guard: limiting is not over-aggressive");
            check (checks, g.consumeLimiting(), "guard: limiting latch set");
            check (checks, ! g.consumeLimiting(), "guard: limiting latch resets on read");

            // 500 ms of quiet signal (50 blocks of 10 ms): the gain must be back above 0.99.
            for (int k = 0; k < 50; ++k)
            {
                fillSine (b, 0.1f, ph);
                g.processBlock (b, m);
            }
            AudioBuffer<float> in (2, blk);
            fillSine (in, 0.1f, ph);
            b.makeCopyOf (in);
            g.processBlock (b, m);
            check (checks, near (b.getMagnitude (0, blk), in.getMagnitude (0, blk), 0.001f),
                   "guard: gain back above 0.99 within 500 ms");
            for (int k = 0; k < 100; ++k)   // another second: well past the snap-to-unity point
            {
                fillSine (b, 0.1f, ph);
                g.processBlock (b, m);
            }
            fillSine (in, 0.1f, ph);
            b.makeCopyOf (in);
            g.processBlock (b, m);
            bool exact = true;
            for (int i = 0; i < blk; ++i) exact = exact && b.getSample (0, i) == in.getSample (0, i);
            check (checks, exact, "guard: after release the signal is bit-exact again (no stall below unity)");
        }

        // NaN / Inf burst -> zeros; the envelope is not poisoned.
        {
            OutputGuardProcessor g; g.setRateAndBufferSizeDetails (sr, blk); g.prepareToPlay (sr, blk);
            AudioBuffer<float> b (2, 8);
            const float nan = std::numeric_limits<float>::quiet_NaN();
            const float inf = std::numeric_limits<float>::infinity();
            const float bad[8] = { nan, inf, -inf, nan, 0.5f, nan, inf, nan };
            for (int i = 0; i < 8; ++i) { b.setSample (0, i, bad[i]); b.setSample (1, i, bad[7 - i]); }
            g.processBlock (b, m);
            bool finite = true;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 8; ++i) finite = finite && std::isfinite (b.getSample (ch, i));
            check (checks, finite, "guard: NaN/Inf are scrubbed to finite output");
            check (checks, b.getSample (0, 0) == 0.0f && b.getSample (0, 1) == 0.0f, "guard: non-finite samples become 0");
            check (checks, g.consumeNonFinite() && ! g.consumeNonFinite(), "guard: non-finite latch sets and resets");

            AudioBuffer<float> q (2, 16);
            for (int i = 0; i < 16; ++i) { q.setSample (0, i, 0.5f); q.setSample (1, i, -0.25f); }
            g.processBlock (q, m);
            check (checks, q.getSample (0, 5) == 0.5f && q.getSample (1, 5) == -0.25f,
                   "guard: signal after a NaN burst is unaffected");
        }

        // Linked stereo: a loud left channel also pulls the right down.
        {
            OutputGuardProcessor g; g.setRateAndBufferSizeDetails (sr, blk); g.prepareToPlay (sr, blk);
            AudioBuffer<float> b (2, 64);
            for (int i = 0; i < 64; ++i) { b.setSample (0, i, 2.0f); b.setSample (1, i, 0.5f); }
            g.processBlock (b, m);
            check (checks, b.getSample (1, 10) < 0.5f - 0.05f, "guard: stereo detector is linked (right is reduced too)");
            check (checks, b.getSample (0, 10) <= ceiling + 1.0e-6f, "guard: loud left held at the ceiling");
        }

        // 3.0 is limited from the very first sample (instant attack, zero latency).
        {
            OutputGuardProcessor g; g.setRateAndBufferSizeDetails (sr, blk); g.prepareToPlay (sr, blk);
            AudioBuffer<float> b (2, 4);
            for (int i = 0; i < 4; ++i) { b.setSample (0, i, 3.0f); b.setSample (1, i, -3.0f); }
            g.processBlock (b, m);
            check (checks, std::abs (b.getSample (0, 0)) <= ceiling && std::abs (b.getSample (1, 0)) <= ceiling,
                   "guard: 3.0 is limited to the ceiling from the first sample");
        }
    }

    std::cout << (failures == 0 ? "  meters ok" : "  meters FAILED") << std::endl;
    return failures;
}
