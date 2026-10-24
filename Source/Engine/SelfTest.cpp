//
//  SelfTest.cpp — see SelfTest.h. Headless runtime checks against real VST3s.
//

#include "SelfTest.h"
#include "GraphController.h"
#include "PresetStore.h"
#include "NodeGeometry.h"
#include "PluginScanWorker.h"
#include <cstdlib>

using namespace juce;

namespace lighthost
{
namespace
{
    struct Report
    {
        int checks = 0, failures = 0;

        void expect (bool condition, const String& message)
        {
            ++checks;
            if (condition)
                std::cout << "  ok   " << message << std::endl;
            else
                { ++failures; std::cout << "  FAIL " << message << std::endl; }
        }
    };

    // Test-only processors (installed through GraphController::instanceFactory) so instrument and
    // multi-output behaviour is checked without any plugin on the machine.
    struct FakeProcessorBase : AudioProcessor
    {
        using AudioProcessor::AudioProcessor;
        const String getName() const override { return "Fake"; }
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}
        double getTailLengthSeconds() const override { return 0.0; }
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

    // 0-in synth: outputs `level` on every output channel while a note is held.
    struct FakeInstrument : FakeProcessorBase
    {
        FakeInstrument (bool stereoOut, float noteLevel)
            : FakeProcessorBase (BusesProperties().withOutput ("Out", stereoOut ? AudioChannelSet::stereo() : AudioChannelSet::mono(), true)),
              stereo (stereoOut), level (noteLevel) {}

        bool isBusesLayoutSupported (const BusesLayout& l) const override
        {
            return l.getMainInputChannelSet().isDisabled()
                && l.getMainOutputChannelSet() == (stereo ? AudioChannelSet::stereo() : AudioChannelSet::mono());
        }
        bool acceptsMidi() const override { return true; }
        void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) override
        {
            int start = 0;
            for (const auto m : midi)
            {
                fill (buffer, start, m.samplePosition);
                start = m.samplePosition;
                if (m.getMessage().isNoteOn())  held = true;
                if (m.getMessage().isNoteOff()) held = false;
            }
            fill (buffer, start, buffer.getNumSamples());
        }
        using AudioProcessor::processBlock;

        void fill (AudioBuffer<float>& b, int from, int to)
        {
            for (int ch = 0; ch < getTotalNumOutputChannels() && ch < b.getNumChannels(); ++ch)
                for (int i = from; i < to; ++i)
                    b.setSample (ch, i, held ? level : 0.0f);
        }
        bool stereo; float level; bool held = false;
    };

    // 2-in/2-out pass-through effect that accepts MIDI (like Graillon or MAutoPitch).
    struct FakeEffect : FakeProcessorBase
    {
        FakeEffect() : FakeProcessorBase (BusesProperties().withInput ("In", AudioChannelSet::stereo(), true)
                                                           .withOutput ("Out", AudioChannelSet::stereo(), true)) {}
        bool acceptsMidi() const override { return true; }
        void processBlock (AudioBuffer<float>&, MidiBuffer&) override {}
        using AudioProcessor::processBlock;
    };

    PluginDescription fakeDescription (const String& name, bool instrument)
    {
        PluginDescription d;
        d.name = name; d.pluginFormatName = "VST3"; d.isInstrument = instrument;
        d.numInputChannels  = instrument ? 0 : 2;
        d.numOutputChannels = 2;
        d.fileOrIdentifier  = "fake:" + name;
        return d;
    }

    // Largest absolute sample of one channel of a block.
    float peakOf (const AudioBuffer<float>& b, int ch)
    {
        return b.getMagnitude (ch, 0, b.getNumSamples());
    }

    // Test X helpers. The scan workers are launched from the app exe: the running exe when it is the app
    // (-run-selftest), the sibling build output when this is LightHostRuntimeTests.
    File findScanWorkerExe()
    {
        const auto self = File::getSpecialLocation (File::currentExecutableFile);
        if (self.getFileName().startsWithIgnoreCase ("LightHost GUI"))
            return self;

        const auto config = self.getParentDirectory();   // .../LightHostRuntimeTests_artefacts/<config>
        return config.getParentDirectory().getParentDirectory().getChildFile ("LightHost_artefacts")
                     .getChildFile (config.getFileName()).getChildFile ("LightHost GUI.exe");
    }

    // Workers inherit the environment; an empty value removes the variable.
    void setEnv (const char* name, const char* value)
    {
       #if JUCE_WINDOWS
        _putenv_s (name, value);
       #else
        if (*value != 0) setenv (name, value, 1); else unsetenv (name);
       #endif
    }

    // Locate loadable stereo FX VST3s. Returns their PluginDescriptions.
    Array<PluginDescription> discoverPlugins (AudioPluginFormatManager& fm,
                                              const StringArray& explicitPaths,
                                              int wanted,
                                              bool instruments = false)
    {
        StringArray candidates = explicitPaths;

        if (candidates.isEmpty())
        {
           #if JUCE_WINDOWS
            File vst3Dir ("C:\\Program Files\\Common Files\\VST3");
           #elif JUCE_MAC
            File vst3Dir ("/Library/Audio/Plug-Ins/VST3");
           #else
            File vst3Dir (File::getSpecialLocation (File::userHomeDirectory).getChildFile (".vst3"));
           #endif

            for (const auto& f : vst3Dir.findChildFiles (File::findFilesAndDirectories, false, "*.vst3"))
                candidates.add (f.getFullPathName());
        }

        Array<PluginDescription> found;

        for (const auto& path : candidates)
        {
            if (found.size() >= wanted)
                break;

            for (auto* format : fm.getFormats())
            {
                OwnedArray<PluginDescription> types;
                format->findAllTypesForFile (types, path);

                for (auto* desc : types)
                {
                    // Effects: stereo in/out. Instruments (on request): any synth with an output.
                    if (instruments ? (desc->isInstrument && desc->numOutputChannels >= 1)
                                    : (! desc->isInstrument
                                       && desc->numInputChannels  >= 2
                                       && desc->numOutputChannels >= 2))
                    {
                        found.add (*desc);
                        break;
                    }
                }

                if (! types.isEmpty())
                    break;   // this path was handled by this format
            }
        }

        return found;
    }

    // Minimal legacy (pre-2026) settings: pluginListActive + the flat
    // plugin-order/bypass-* keys, exactly as upstream LightHost wrote them.
    void writeLegacySettings (PropertiesFile& settings,
                              const Array<PluginDescription>& plugins,
                              const std::vector<bool>& bypass)
    {
        KnownPluginList active;
        for (const auto& p : plugins)
            active.addType (p);

        if (auto xml = active.createXml())
            settings.setValue ("pluginListActive", xml.get());

        const auto key = [] (const String& kind, const PluginDescription& p)
        {
            return "plugin-" + kind + "-" + p.name + p.version + p.pluginFormatName;
        };

        for (int i = 0; i < plugins.size(); ++i)
        {
            const auto& p = plugins.getReference (i);
            settings.setValue (key ("order",  p), String (1000 + i));
            settings.setValue (key ("bypass", p), bypass[(size_t) i]);
        }

        settings.saveIfNeeded();
    }

    File tempSettingsFile (const String& name)
    {
        return File::getSpecialLocation (File::tempDirectory)
                   .getChildFile ("LightHostSelfTest_" + name + ".settings");
    }

    std::unique_ptr<PropertiesFile> makeSettings (const File& f)
    {
        PropertiesFile::Options o;
        o.commonToAllUsers = false;
        return std::make_unique<PropertiesFile> (f, o);
    }

    // Graph + controller + settings file for one runtime test block; the temp settings file
    // is deleted on destruction. Declaration order = construction order.
    struct GraphFixture
    {
        GraphFixture (AudioPluginFormatManager& fm, const String& name, double sr = 44100.0, int blockSize = 512,
                      int numOutputs = 2)
            : file (tempSettingsFile (name))
        {
            file.deleteFile();
            graph.setPlayConfigDetails (2, numOutputs, sr, blockSize);
            graph.prepareToPlay (sr, blockSize);
            settings   = makeSettings (file);
            controller = std::make_unique<GraphController> (graph, fm);
            controller->loadFrom (*settings);
        }

        ~GraphFixture()
        {
            controller.reset();
            settings.reset();
            file.deleteFile();
        }

        File file;
        std::unique_ptr<PropertiesFile> settings;
        AudioProcessorGraph graph;
        std::unique_ptr<GraphController> controller;
    };

    // Fill the stereo block with white noise, process `count` blocks through the
    // graph, and return the highest ch0 peak the meter saw. The meter is re-fetched
    // per block via `meterProvider` so a caller can keep driving across graph edits
    // (meters are re-spliced on every rewire). Processing many blocks lets any
    // plugin latency fill through before the peak is sampled. `onBlock` (optional) runs
    // after every processed block, for callers that sample several meters themselves.
    float driveNoiseBlocks (AudioProcessorGraph& graph, AudioBuffer<float>& block, MidiBuffer& midi,
                            Random& rng, const std::function<const MeterTap*()>& meterProvider, int count,
                            const std::function<void()>& onBlock = {})
    {
        float maxPeak = 0.0f;
        for (int b = 0; b < count; ++b)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                float* d = block.getWritePointer (ch);
                for (int i = 0; i < block.getNumSamples(); ++i)
                    d[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.5f;
            }
            midi.clear();
            graph.processBlock (block, midi);
            if (const MeterTap* m = meterProvider())
                maxPeak = jmax (maxPeak, m->read().peak[0]);
            if (onBlock)
                onBlock();
        }
        return maxPeak;
    }

    //==========================================================================
    // Plugin capability report. Runs for each explicitly-passed plugin path to
    // characterise a plugin that misbehaves in the chain (bus layout, latency,
    // params, and whether it actually alters / outputs a test tone). Purely
    // diagnostic — does not affect the pass/fail count.
    void diagnosePlugin (AudioPluginFormatManager& fm, const String& path)
    {
        std::cout << "\n================ PLUGIN DIAGNOSTIC ================\n";
        std::cout << "Path: " << path << std::endl;

        PluginDescription desc;
        bool found = false;
        for (auto* format : fm.getFormats())
        {
            OwnedArray<PluginDescription> types;
            format->findAllTypesForFile (types, path);
            if (! types.isEmpty()) { desc = *types[0]; found = true; break; }
        }
        if (! found) { std::cout << "  could not identify a plugin at that path\n"; return; }

        const double sr = 48000.0;
        const int    block = 512;
        String err;
        auto inst = fm.createPluginInstance (desc, sr, block, err);
        if (inst == nullptr) { std::cout << "  createPluginInstance FAILED: " << err << "\n"; return; }

        auto busReport = [&] (bool input)
        {
            const int n = inst->getBusCount (input);
            std::cout << "  " << (input ? "in " : "out") << " buses=" << n;
            for (int i = 0; i < n; ++i)
                if (auto* bus = inst->getBus (input, i))
                    std::cout << "  [" << i << " " << bus->getCurrentLayout().getDescription()
                              << (bus->isEnabled() ? "" : " DISABLED") << "]";
            std::cout << "  totalCh=" << (input ? inst->getTotalNumInputChannels()
                                                : inst->getTotalNumOutputChannels()) << "\n";
        };

        std::cout << "Name: " << inst->getName() << "  format=" << desc.pluginFormatName
                  << "  isInstrument=" << (desc.isInstrument ? 1 : 0) << "\n";
        std::cout << "BEFORE forceStereo:\n"; busReport (true); busReport (false);

        const bool nowStereo = lighthost::buses::forceStereo (*inst, sr, block);
        std::cout << "forceStereo -> " << (nowStereo ? "2-in/2-out OK" : "NOT 2/2") << "\n";
        std::cout << "AFTER forceStereo:\n"; busReport (true); busReport (false);

        inst->prepareToPlay (sr, block);
        std::cout << "latencySamples=" << inst->getLatencySamples()
                  << "  tailSeconds=" << inst->getTailLengthSeconds() << "\n";

        auto ps = lighthost::params::collect (*inst, false);
        std::cout << "params (" << ps.size() << "):\n";
        for (size_t i = 0; i < ps.size() && i < 30; ++i)
            std::cout << "  [" << ps[i].index << "] " << ps[i].name << " = " << ps[i].value
                      << " '" << ps[i].text << "'" << (ps[i].automatable ? "" : " (non-autom)") << "\n";

        // Drive DISTINCT L/R sines (L=445, R=311 Hz) long enough to exceed latency,
        // so a stereo->mono collapse (outL==outR) or a dead channel (outR~0) shows up.
        const int nCh     = jmax (2, inst->getTotalNumInputChannels(), inst->getTotalNumOutputChannels());
        const int latency = inst->getLatencySamples();
        const int warm    = jmax (128, latency / block + 64);
        auto fillDistinct = [] (AudioBuffer<float>& buf, double& pL, double& pR, double dL, double dR)
        {
            const int n = buf.getNumSamples();
            for (int i = 0; i < n; ++i)
            {
                const float l = 0.5f * (float) std::sin (pL); pL += dL;
                const float r = 0.5f * (float) std::sin (pR); pR += dR;
                if (buf.getNumChannels() > 0) buf.setSample (0, i, l);
                if (buf.getNumChannels() > 1) buf.setSample (1, i, r);
                for (int ch = 2; ch < buf.getNumChannels(); ++ch) buf.setSample (ch, i, 0.0f);
            }
        };
        const double dL = 2.0 * MathConstants<double>::pi * 445.0 / sr;
        const double dR = 2.0 * MathConstants<double>::pi * 311.0 / sr;

        {   // (a) plugin processed DIRECTLY
            AudioBuffer<float> buffer (nCh, block);
            MidiBuffer midi;
            double pL = 0.0, pR = 0.0;
            float outL = 0, outR = 0, diffLR = 0;
            for (int b = 0; b < warm; ++b)
            {
                fillDistinct (buffer, pL, pR, dL, dR);
                midi.clear();
                inst->processBlock (buffer, midi);
                outL = buffer.getRMSLevel (0, 0, block);
                outR = nCh > 1 ? buffer.getRMSLevel (1, 0, block) : 0.0f;
                if (nCh > 1) { float s = 0; for (int i = 0; i < block; ++i) s += std::abs (buffer.getSample(0,i) - buffer.getSample(1,i)); diffLR = s / block; }
            }
            std::cout << "DIRECT (L=445,R=311): out RMS L=" << outL << " R=" << outR
                      << "  mean|L-R|=" << diffLR << "\n";
            std::cout << (outL < 1e-4f && outR < 1e-4f ? "  => NO OUTPUT\n"
                        : (outL < 1e-4f || outR < 1e-4f ? "  => ONE CHANNEL DEAD\n"
                        : (diffLR < 1e-4f ? "  => STEREO COLLAPSED TO MONO\n" : "  => stereo preserved\n")));
            inst->releaseResources();
        }

        {   // (b) plugin through the real GraphController (app's routing incl. strip/meter)
            AudioProcessorGraph graph;
            graph.setPlayConfigDetails (2, 2, sr, block);
            graph.prepareToPlay (sr, block);
            GraphController controller (graph, fm);
            const File f = tempSettingsFile ("diag");
            f.deleteFile();
            auto settings = makeSettings (f);
            controller.loadFrom (*settings);
            const String uid = controller.appendToChain (desc);
            AudioBuffer<float> buffer (2, block);
            MidiBuffer midi;
            double pL = 0.0, pR = 0.0;
            float outL = 0, outR = 0, diffLR = 0;
            for (int b = 0; b < warm; ++b)
            {
                fillDistinct (buffer, pL, pR, dL, dR);
                midi.clear();
                graph.processBlock (buffer, midi);
                outL = buffer.getRMSLevel (0, 0, block);
                outR = buffer.getRMSLevel (1, 0, block);
                { float s = 0; for (int i = 0; i < block; ++i) s += std::abs (buffer.getSample(0,i) - buffer.getSample(1,i)); diffLR = s / block; }
            }
            std::cout << "VIA GRAPH (in->plugin->strip->out): out RMS L=" << outL << " R=" << outR
                      << "  mean|L-R|=" << diffLR << "  uid=" << (uid.isNotEmpty() ? "ok" : "EMPTY") << "\n";
            std::cout << (outL < 1e-4f && outR < 1e-4f ? "  => NO OUTPUT via graph\n"
                        : (outL < 1e-4f || outR < 1e-4f ? "  => ONE CHANNEL DEAD via graph\n"
                        : (diffLR < 1e-4f ? "  => STEREO COLLAPSED TO MONO via graph\n" : "  => stereo preserved via graph\n")));
            f.deleteFile();
        }
        std::cout << "===================================================\n";
    }
}

//==============================================================================
int runSelfTest (const StringArray& explicitPluginPaths)
{
    AudioPluginFormatManager formatManager;
    addDefaultFormatsToManager (formatManager);   // JUCE 8: free function replaces addDefaultFormats()

    std::cout << "GraphController runtime self-test (real VST3 plugins)" << std::endl;

    // Explicitly-passed plugin paths get a capability report first (diagnostic for
    // "plugin X misbehaves in the chain"); auto-discovery runs are unaffected.
    for (const auto& p : explicitPluginPaths)
        diagnosePlugin (formatManager, p);

    Report r;

    //==========================================================================
    // Test L — mono input: sums a one-sided source onto both output channels.
    //          Needs no plugin (bare in -> out passthrough), so it always runs.
    //==========================================================================
    std::cout << "\n[L] mono input sums to both channels" << std::endl;
    {
        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, 44100.0, 512);
        graph.prepareToPlay (44100.0, 512);

        GraphController controller (graph, formatManager);
        const File f = tempSettingsFile ("mono");
        f.deleteFile();
        auto settings = makeSettings (f);
        controller.loadFrom (*settings);   // empty document -> in -> out passthrough

        AudioBuffer<float> block (2, 512);
        MidiBuffer midi;
        // A mono source arriving only on the RIGHT input channel (left silent).
        auto drive = [&] (float& lRms, float& rRms)
        {
            for (int i = 0; i < 512; ++i) { block.setSample (0, i, 0.0f); block.setSample (1, i, 0.4f); }
            midi.clear();
            graph.processBlock (block, midi);
            lRms = block.getRMSLevel (0, 0, 512);
            rRms = block.getRMSLevel (1, 0, 512);
        };

        float l = 0, rr = 0;
        controller.setMonoInput (false);
        drive (l, rr);
        r.expect (l < 1.0e-4f && rr > 0.0f, "mono OFF: right-only input stays right-only (left silent)");

        controller.setMonoInput (true);
        r.expect (controller.isMonoInput(), "isMonoInput reflects the enabled setting");
        drive (l, rr);
        r.expect (l > 0.0f && rr > 0.0f, "mono ON: right-only input now carries on BOTH channels");
        r.expect (std::abs (l - rr) < 1.0e-4f, "mono ON: both output channels are equal");

        controller.setMonoInput (false);
        drive (l, rr);
        r.expect (l < 1.0e-4f && rr > 0.0f, "mono toggled back OFF: left silent again (live re-splice)");

        f.deleteFile();
    }

    //==========================================================================
    // Baseline — zero cost when unused (v1.2 B). A serial graph with one OUT and no instrument must keep
    //            the exact node and connection counts of v1.1. The numbers were recorded on the engine
    //            before B1; they must hold after every B commit. Needs no installed plugin: the nodes
    //            are stand-ins for plugins that don't exist.
    //==========================================================================
    std::cout << "\n[Baseline] render sequence of a plain serial graph" << std::endl;
    {
        GraphFixture fx (formatManager, "baseline");
        GraphController& controller = *fx.controller;
        AudioProcessorGraph& graph = fx.graph;

        PluginDescription ghost;
        ghost.name = "GhostFx"; ghost.pluginFormatName = "VST3"; ghost.numInputChannels = 2; ghost.numOutputChannels = 2;
        ghost.fileOrIdentifier = File::getSpecialLocation (File::tempDirectory).getChildFile ("lighthost_no_such_ghost.vst3").getFullPathName();

        const int emptyNodes = graph.getNumNodes(), emptyConns = (int) graph.getConnections().size();
        std::cout << "  baseline empty: nodes " << emptyNodes << ", connections " << emptyConns << std::endl;

        controller.appendToChain (ghost);
        controller.appendToChain (ghost);
        const int nodes = graph.getNumNodes(), conns = (int) graph.getConnections().size();
        std::cout << "  baseline IN->2 plugins->OUT: nodes " << nodes << ", connections " << conns << std::endl;
        r.expect (emptyNodes == 5 && emptyConns == 8, "baseline: empty graph counts");
        r.expect (nodes == 9 && conns == 16, "baseline: two-plugin serial graph counts");

        // The same graph with the OUT pinned to the first active pair of an open device is the same render sequence.
        lighthost::outputs::Layout layout;
        layout.known = true; layout.deviceName = "BaselineDevice"; layout.activeMask.parseString ("1100", 2);
        layout.names = StringArray { "Out 1", "Out 2", "Loop-back 1", "Loop-back 2" };
        controller.setOutputLayout (layout);
        controller.pinLegacyOutputs (*lighthost::outputs::pinTarget (layout));
        r.expect (graph.getNumNodes() == 9 && (int) graph.getConnections().size() == 16,
                  "baseline: a pinned OUT keeps the exact node and connection counts");
    }

    //==========================================================================
    // Test U (fake processors) — instrument nodes. A 0-input synth is a SOURCE: it sits beside the serial
    //          chain wired to the main OUT, receives MIDI, takes no edge in and no dry/wet mix, and its note
    //          reaches the output (a mono synth on both sides). New effects start with MIDI off. The chain
    //          ops keep the instrument's edge. A missing instrument is silent. Adding one costs only the
    //          node, its strip and the MIDI input node.
    //==========================================================================
    std::cout << "\n[U] instrument nodes (fake processors)" << std::endl;
    {
        const auto synthDesc = fakeDescription ("FakeSynth", true);
        const auto fxDesc    = fakeDescription ("FakeFx", false);

        GraphFixture fx (formatManager, "instrument");
        GraphController& controller = *fx.controller;
        AudioProcessorGraph& graph = fx.graph;
        controller.instanceFactory = [] (const PluginDescription& d) -> std::unique_ptr<AudioProcessor>
        {
            if (d.name == "FakeSynth")  return std::make_unique<FakeInstrument> (true, 0.5f);
            if (d.name == "FakeFx")     return std::make_unique<FakeEffect>();
            return nullptr;   // anything else can't load
        };

        const String outUid = controller.document.getIoNodeUid (false);
        const auto hasPair = [&controller] (const String& a, const String& b)
        {
            return controller.document.hasConnection (a, 0, b, 0) && controller.document.hasConnection (a, 1, b, 1);
        };
        const auto render = [&graph] (bool noteOn, AudioBuffer<float>& block)
        {
            block.clear();
            MidiBuffer midi;
            if (noteOn)
                midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 0);
            graph.processBlock (block, midi);
        };

        const int nodesBefore = graph.getNumNodes();
        int before = controller.getPublishCount();
        const String piano = controller.addPlugin (synthDesc);
        r.expect (piano.isNotEmpty() && ! controller.isMissing (piano), "the instrument is added and live");
        r.expect (controller.getPublishCount() - before == 1, "adding an instrument publishes exactly once");
        r.expect (graph.getNumNodes() - nodesBefore == 3, "an instrument costs its node, its strip and the MIDI input node only");
        r.expect (controller.document.isSourceNode (piano), "the node reads as a source");
        r.expect (hasPair (piano, outUid), "the instrument is wired to the main OUT");
        r.expect (controller.getNodeReceivesMidi (piano), "an instrument receives MIDI");
        r.expect (controller.isLinearChain() && controller.getChain().empty(), "an instrument leaves the serial chain empty and linear");

        const String e1 = controller.addPlugin (fxDesc);
        const String e2 = controller.addPlugin (fxDesc);
        r.expect (e1.isNotEmpty() && e2.isNotEmpty(), "two effects appended after the instrument");
        r.expect (! controller.getNodeReceivesMidi (e1) && ! controller.getNodeReceivesMidi (e2), "new effects start with MIDI off");
        r.expect (hasPair (controller.document.getIoNodeUid (true), e1) && hasPair (e1, e2) && hasPair (e2, outUid),
                  "the effects form the serial chain IN -> e1 -> e2 -> OUT");
        r.expect (hasPair (piano, outUid), "appending keeps the instrument's edge");
        controller.moveDown (e1);
        r.expect (hasPair (e2, e1) && hasPair (e1, outUid) && hasPair (piano, outUid), "Move Down keeps the instrument's edge");
        controller.moveUp (e1);
        r.expect (hasPair (e1, e2) && hasPair (piano, outUid), "Move Up keeps the instrument's edge");
        r.expect (! controller.canConnectPair (e1, piano) && ! controller.connect (controller.document.getIoNodeUid (true), 0, piano, 0),
                  "no connection INTO an instrument");

        before = controller.getPublishCount();
        controller.setNodeMix (piano, 0.3f);
        r.expect (controller.getPublishCount() == before && controller.getNodeMix (piano) == 1.0f, "mix on an instrument is a no-op");

        AudioBuffer<float> block (2, 512);
        render (false, block);
        r.expect (peakOf (block, 0) < 1.0e-6f && peakOf (block, 1) < 1.0e-6f, "silent before a note");
        render (true, block);
        r.expect (peakOf (block, 0) > 0.2f && peakOf (block, 1) > 0.2f, "a MIDI note reaches both output channels through the strip");
        r.expect (std::abs (peakOf (block, 0) - peakOf (block, 1)) < 1.0e-4f, "centre pan: both channels equal");

        // Replacing an effect by an instrument drops the edges that fed it.
        const auto swapped = controller.replacePlugin (e2, synthDesc);
        r.expect (swapped.ok && controller.document.isSourceNode (e2), "an effect can be replaced by an instrument");
        bool anyInto = false;
        for (const auto& c : controller.getConnections())
            anyInto = anyInto || c.dstUid == e2;
        r.expect (! anyInto, "replacing by an instrument removes the edges into the node");
    }
    {
        GraphFixture fx (formatManager, "monosynth");
        GraphController& controller = *fx.controller;
        controller.instanceFactory = [] (const PluginDescription& d) -> std::unique_ptr<AudioProcessor>
        {
            if (d.name == "MonoSynth") return std::make_unique<FakeInstrument> (false, 0.5f);
            return nullptr;
        };
        const String mono = controller.addPlugin (fakeDescription ("MonoSynth", true));
        auto* node = controller.getNodeForUid (mono);
        r.expect (node != nullptr && node->getProcessor()->getTotalNumOutputChannels() == 1, "the mono synth stays 1-out");

        AudioBuffer<float> block (2, 512);
        block.clear();
        MidiBuffer midi;
        midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 0);
        fx.graph.processBlock (block, midi);
        r.expect (peakOf (block, 0) > 0.2f && std::abs (peakOf (block, 0) - peakOf (block, 1)) < 1.0e-4f,
                  "a mono synth is heard on both channels");
    }
    {
        GraphFixture fx (formatManager, "ghostsynth");
        GraphController& controller = *fx.controller;
        controller.instanceFactory = [] (const PluginDescription&) -> std::unique_ptr<AudioProcessor> { return nullptr; };
        const String ghost = controller.addPlugin (fakeDescription ("GhostSynth", true));
        r.expect (ghost.isNotEmpty() && controller.isMissing (ghost) && controller.document.isSourceNode (ghost),
                  "a missing instrument is a stand-in that still reads as a source");
        r.expect (! controller.canConnectPair (controller.document.getIoNodeUid (true), ghost), "nothing can feed a missing instrument");

        AudioBuffer<float> block (2, 512);
        block.clear();
        MidiBuffer midi;
        midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 0);
        fx.graph.processBlock (block, midi);
        r.expect (peakOf (block, 0) < 1.0e-6f && peakOf (block, 1) < 1.0e-6f, "a missing instrument is silent");
    }

    //==========================================================================
    // Test W (fake processors) — output nodes bound to device channel pairs. A 4-channel graph: source A on the
    //          main OUT (pair 0/1), source B on an extra OUT bound to pair 2/3 — each pair carries only its own
    //          source; one source on both OUTs shows on both; an inactive pair is silent and unwired; the guard
    //          limits every pair; add / rebind / layout change publish once. Legacy scenario (Hung's setup):
    //          the original OUT on the first active pair of a 2-channel device is pinned to channel index 2,
    //          the wiring doesn't change, and enabling Out 1/2 afterwards leaves it on Loop-back. A binding
    //          belongs to its device: on another device the main OUT plays on the first active pair, the
    //          extra OUT is silent.
    //==========================================================================
    std::cout << "\n[W] output nodes bound to device channel pairs" << std::endl;
    {
        using lighthost::outputs::Layout;
        const auto layoutOf = [] (const String& device, const char* bits)
        {
            Layout l;
            l.known = true;
            l.deviceName = device;
            l.activeMask.parseString (bits, 2);
            l.names = StringArray { "Out 1", "Out 2", "Loop-back 1", "Loop-back 2" };
            return l;
        };
        const auto factory = [] (const PluginDescription& d) -> std::unique_ptr<AudioProcessor>
        {
            if (d.name == "SynthA")     return std::make_unique<FakeInstrument> (true, 0.5f);
            if (d.name == "SynthB")     return std::make_unique<FakeInstrument> (true, 0.25f);
            if (d.name == "LoudSynth")  return std::make_unique<FakeInstrument> (true, 2.0f);
            return nullptr;
        };
        const auto renderN = [] (AudioProcessorGraph& graph, int channels, AudioBuffer<float>& block)
        {
            block.setSize (channels, 512, false, true, true);
            block.clear();
            MidiBuffer midi;
            midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 0);
            graph.processBlock (block, midi);
        };
        const auto near = [] (float a, float b, float tol = 0.02f) { return std::abs (a - b) <= tol; };
        const auto level = [] (const AudioBuffer<float>& b, int ch) { return b.getMagnitude (ch, 0, b.getNumSamples()); };

        // --- strict bindings on a 4-channel graph ------------------------------------------
        {
            GraphFixture fx (formatManager, "outputs4", 44100.0, 512, 4);
            GraphController& controller = *fx.controller;
            AudioProcessorGraph& graph = fx.graph;
            controller.instanceFactory = factory;
            const String mainUid = controller.document.getIoNodeUid (false);

            int before = controller.getPublishCount();
            controller.setOutputLayout (layoutOf ("EVO", "1111"));
            r.expect (controller.getPublishCount() == before, "a layout that doesn't move any OUT publishes nothing");
            r.expect (controller.getOutputBase (mainUid) == 0, "the unbound main OUT plays on the first active pair");

            const String a = controller.addPlugin (fakeDescription ("SynthA", true));
            const String b = controller.addPlugin (fakeDescription ("SynthB", true));
            r.expect (a.isNotEmpty() && b.isNotEmpty(), "two sources added");

            const int nodesBefore = graph.getNumNodes(), connsBefore = (int) graph.getConnections().size();
            before = controller.getPublishCount();
            const String out2 = controller.addOutput (2);
            r.expect (out2.isNotEmpty() && controller.getPublishCount() - before == 1, "adding an OUT publishes exactly once");
            r.expect (controller.getOutputDisplayLabel (out2) == "Loop-back 1/2", "the new OUT is labelled with its pair");
            r.expect (graph.getNumNodes() - nodesBefore == 1 && (int) graph.getConnections().size() == connsBefore,
                      "an OUT nothing feeds costs only its IO node (no guard, no meter, no edges)");
            r.expect (controller.getOutputMeter (out2) == nullptr && controller.getOutputMeter() != nullptr, "only the fed OUT has a meter");

            controller.disconnectPair (b, mainUid);
            controller.connectPair (b, out2);
            r.expect (controller.getOutputMeter (out2) != nullptr, "feeding the OUT gives it its guard and meter");

            AudioBuffer<float> block;
            renderN (graph, 4, block);
            r.expect (near (level (block, 0), 0.5f) && near (level (block, 1), 0.5f), "pair 0/1 carries source A only");
            r.expect (near (level (block, 2), 0.25f) && near (level (block, 3), 0.25f), "pair 2/3 carries source B only");

            // One source on both OUTs.
            controller.disconnectPair (b, out2);
            controller.connectPair (a, out2);
            renderN (graph, 4, block);
            r.expect (near (level (block, 0), 0.5f) && near (level (block, 2), 0.5f) && near (level (block, 3), 0.5f),
                      "one source wired to both OUTs appears on both pairs");

            // An inactive pair is silent and unwired; the other pair is untouched.
            before = controller.getPublishCount();
            controller.setOutputLayout (layoutOf ("EVO", "0011"));
            r.expect (controller.getPublishCount() - before == 1, "a layout change that silences an OUT publishes exactly once");
            r.expect (! controller.isOutputActive (out2) && controller.isOutputActive (mainUid), "the OUT on the inactive pair reports inactive");
            renderN (graph, 4, block);
            r.expect (near (level (block, 0), 0.5f) && level (block, 2) < 1.0e-6f && level (block, 3) < 1.0e-6f,
                      "an inactive pair is silent, the active one unaffected");
            before = controller.getPublishCount();
            controller.setOutputLayout (layoutOf ("EVO", "0011"));
            r.expect (controller.getPublishCount() == before, "the same layout again publishes nothing");
            controller.setOutputLayout (layoutOf ("EVO", "1111"));
            renderN (graph, 4, block);
            r.expect (near (level (block, 2), 0.5f), "the pair comes back when it is active again");

            // Rebind: out2 onto pair 0/1 sums with the main OUT; back to 2/3.
            before = controller.getPublishCount();
            controller.setOutputBinding (out2, 0);
            r.expect (controller.getPublishCount() - before == 1 && controller.getOutputDisplayLabel (out2) == "Out 1/2",
                      "rebinding publishes exactly once and updates the label");
            renderN (graph, 4, block);
            r.expect (level (block, 0) > 0.9f && level (block, 2) < 1.0e-6f, "after the rebind both OUTs sum on pair 0/1 and pair 2/3 is silent");
            controller.setOutputBinding (out2, 2);
            before = controller.getPublishCount();
            controller.setOutputBinding (out2, 2);
            r.expect (controller.getPublishCount() == before, "rebinding to the same pair is a no-op");

            // The guard limits every pair.
            const String loud = controller.addPlugin (fakeDescription ("LoudSynth", true));
            controller.connectPair (loud, out2);
            renderN (graph, 4, block);
            const float ceiling = OutputGuardProcessor::getCeiling();
            bool allLimited = true;
            for (int ch = 0; ch < 4; ++ch)
                allLimited = allLimited && level (block, ch) <= ceiling + 0.01f && level (block, ch) > 0.5f;
            r.expect (allLimited, "the output guard limits both pairs");

            // Main and IN are permanent; an extra OUT can go.
            const int outsBefore = (int) controller.document.getOutputUids().size();
            controller.removeNode (mainUid);
            controller.removeNode (controller.document.getIoNodeUid (true));
            r.expect ((int) controller.document.getOutputUids().size() == outsBefore && controller.getNodeForUid (mainUid) != nullptr,
                      "the main OUT and IN can't be removed");
            before = controller.getPublishCount();
            controller.removeNode (out2);
            r.expect (controller.getPublishCount() - before == 1 && (int) controller.document.getOutputUids().size() == outsBefore - 1
                      && controller.getNodeForUid (out2) == nullptr, "an extra OUT is removed with one publish");
            renderN (graph, 4, block);
            r.expect (level (block, 0) > 0.5f && level (block, 2) < 1.0e-6f, "after removing it only the main pair plays");
        }

        // --- impossible pairs are refused and repaired (B-R4) ----------------------------------
        {
            GraphFixture fx (formatManager, "outputsrepair", 44100.0, 512, 4);
            GraphController& controller = *fx.controller;
            controller.instanceFactory = factory;
            controller.setOutputLayout (layoutOf ("EVO", "1111"));   // 4 channels: pairs 0/1 and 2/3
            const String mainUid = controller.document.getIoNodeUid (false);
            const String a = controller.addPlugin (fakeDescription ("SynthA", true));

            const int outsBefore = (int) controller.document.getOutputUids().size();
            const int before = controller.getPublishCount();
            r.expect (controller.addOutput (3).isEmpty() && controller.addOutput (-2).isEmpty() && controller.addOutput (1684767600).isEmpty(),
                      "addOutput refuses a pair that isn't on the device");
            const String ok = controller.addOutput (2);
            r.expect (ok.isNotEmpty(), "addOutput accepts the last pair of the device");
            const int afterAdd = controller.getPublishCount();
            controller.setOutputBinding (ok, 3);
            controller.setOutputBinding (ok, 1684767600);
            r.expect (controller.getPublishCount() == afterAdd && controller.document.getOutputChannel (ok) == 2,
                      "setOutputBinding refuses a pair that isn't on the device");
            r.expect ((int) controller.document.getOutputUids().size() == outsBefore + 1 && afterAdd - before == 1,
                      "refused calls add no node and publish nothing");

            // A document with a stray OUT (as in Hung's settings) is repaired when it is loaded.
            GraphDocument broken = controller.snapshotDocument();
            const String stray = broken.addOutputNode (1684767600, "", "EVO", 100.0f, 900.0f);
            broken.addConnection (a, 0, stray, 0);
            broken.addConnection (a, 1, stray, 1);
            const int edgesWithStray = broken.getConnections().getNumChildren();
            controller.loadDocument (broken);
            r.expect (controller.document.getNodeByUid (stray).isValid() == false && controller.getNodeForUid (stray) == nullptr,
                      "loadDocument removes the stray OUT");
            r.expect (controller.document.getConnections().getNumChildren() == edgesWithStray - 2, "its cables went with it");
            r.expect (controller.document.getNodeByUid (mainUid).isValid() && controller.getNodeForUid (mainUid) != nullptr
                      && controller.document.getNodeByUid (ok).isValid(), "the main OUT and the valid extra OUT stay");
        }

        // --- Hung's legacy setup: the original OUT on Loop-back, then Out 1/2 gets enabled ---
        {
            GraphFixture fx (formatManager, "outputslegacy", 44100.0, 512, 2);
            GraphController& controller = *fx.controller;
            AudioProcessorGraph& graph = fx.graph;
            controller.instanceFactory = factory;
            const String mainUid = controller.document.getIoNodeUid (false);

            const auto evo2 = layoutOf ("EVO", "1100");   // only the Loop-back pair is active
            controller.setOutputLayout (evo2);
            const String a = controller.addPlugin (fakeDescription ("SynthA", true));
            AudioBuffer<float> block;
            renderN (graph, 2, block);
            r.expect (near (level (block, 0), 0.5f) && near (level (block, 1), 0.5f), "legacy: the unbound OUT plays on the first active pair");

            const int nodes0 = graph.getNumNodes(), conns0 = (int) graph.getConnections().size();
            const int before = controller.getPublishCount();
            const auto pin = lighthost::outputs::pinTarget (evo2);
            r.expect (pin.has_value() && controller.pinLegacyOutputs (*pin), "pinning the legacy OUT reports a change");
            r.expect (controller.document.getOutputChannel (mainUid) == 2 && controller.document.getOutputLabel (mainUid) == "Loop-back 1/2"
                      && controller.document.getOutputDevice (mainUid) == "EVO", "legacy: the main OUT is pinned to channel index 2 / Loop-back 1/2 / EVO");
            r.expect (controller.getPublishCount() == before, "pinning rewires nothing (same pair, same wiring)");
            r.expect (graph.getNumNodes() == nodes0 && (int) graph.getConnections().size() == conns0,
                      "the render sequence has the same node and connection count after pinning");
            r.expect (! controller.pinLegacyOutputs (*pin), "pinning twice changes nothing");
            renderN (graph, 2, block);
            r.expect (near (level (block, 0), 0.5f), "legacy: still audible on the same pair after pinning");

            // Hung enables Out 1/2: the device now has 4 active outputs.
            graph.setPlayConfigDetails (2, 4, 44100.0, 512);
            graph.prepareToPlay (44100.0, 512);
            const int beforeEnable = controller.getPublishCount();
            controller.setOutputLayout (layoutOf ("EVO", "1111"));
            r.expect (controller.getPublishCount() - beforeEnable == 1, "enabling Out 1/2 rewires once");
            r.expect (controller.getOutputBase (mainUid) == 2, "the pinned OUT now lands on graph channel 2");
            renderN (graph, 4, block);
            r.expect (level (block, 0) < 1.0e-6f && level (block, 1) < 1.0e-6f, "legacy: nothing moved onto Out 1/2");
            r.expect (near (level (block, 2), 0.5f) && near (level (block, 3), 0.5f), "legacy: the feed stays on Loop-back (device channels 3/4)");

            // A monitor OUT on Out 1/2 next to it.
            const String monitor = controller.addOutput (0);
            controller.connectPair (a, monitor);
            renderN (graph, 4, block);
            r.expect (near (level (block, 0), 0.5f) && near (level (block, 2), 0.5f), "the monitor OUT plays on Out 1/2, Loop-back is unchanged");

            // Another device: bindings made on EVO don't apply.
            controller.setOutputLayout (layoutOf ("Other interface", "1111"));
            renderN (graph, 4, block);
            r.expect (controller.getOutputBase (mainUid) == 0 && ! controller.isOutputActive (monitor),
                      "other device: the main OUT falls back to the first active pair, the extra OUT is inactive");
            r.expect (near (level (block, 0), 0.5f) && level (block, 2) < 1.0e-6f && level (block, 3) < 1.0e-6f,
                      "other device: the main OUT plays on pair 0/1, the extra OUT is silent");
            r.expect (controller.document.getOutputChannel (mainUid) == 2 && controller.document.getOutputDevice (monitor) == "EVO",
                      "other device: the stored bindings are not rewritten");

            // Back on the original device the strict bindings return.
            controller.setOutputLayout (layoutOf ("EVO", "1111"));
            renderN (graph, 4, block);
            r.expect (near (level (block, 0), 0.5f) && near (level (block, 2), 0.5f), "back on EVO both bindings apply again");

            // Pinning on the other device never overwrites the EVO binding.
            r.expect (! controller.pinLegacyOutputs (*lighthost::outputs::pinTarget (layoutOf ("Other interface", "1111"))),
                      "pinning on another device doesn't touch bound OUTs");
        }

        // --- no device yet: a pinned main OUT still plays (first pair), nothing crashes ----------
        {
            GraphFixture fx (formatManager, "outputsnodevice", 44100.0, 512, 2);
            GraphController& controller = *fx.controller;
            controller.instanceFactory = factory;
            const String mainUid = controller.document.getIoNodeUid (false);
            controller.document.setOutputBinding (mainUid, 2, "Loop-back 1/2", "EVO");
            controller.addPlugin (fakeDescription ("SynthA", true));
            AudioBuffer<float> block;
            renderN (fx.graph, 2, block);
            r.expect (near (level (block, 0), 0.5f), "no device known: the main OUT plays on the first pair");
            r.expect (controller.addOutput (0).isEmpty(), "no device known: an OUT can't be added");
        }
    }

    const Array<PluginDescription> plugins = discoverPlugins (formatManager, explicitPluginPaths, 2);

    if (plugins.isEmpty())
    {
        std::cout << "\nSKIP: no loadable stereo VST3 found — plugin-dependent tests skipped." << std::endl;
        std::cout << "\n" << (r.failures == 0 ? "PASS " : "FAIL ")
                  << (r.checks - r.failures) << "/" << r.checks << " checks" << std::endl;
        std::cout.flush();
        return r.failures == 0 ? 0 : 1;
    }

    const PluginDescription& pA = plugins.getReference (0);
    std::cout << "Using plugin A: " << pA.name << " (" << pA.pluginFormatName << ")" << std::endl;
    if (plugins.size() > 1)
        std::cout << "Using plugin B: " << plugins.getReference (1).name << std::endl;

    //==========================================================================
    // Test A — two instances of the SAME plugin coexist as distinct live nodes.
    //==========================================================================
    std::cout << "\n[A] duplicate-plugin coexistence" << std::endl;
    {
        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, 44100.0, 512);
        graph.prepareToPlay (44100.0, 512);

        GraphController controller (graph, formatManager);
        const File f = tempSettingsFile ("dup");
        f.deleteFile();
        auto settings = makeSettings (f);
        controller.loadFrom (*settings);

        const String uid1 = controller.appendToChain (pA);
        const String uid2 = controller.appendToChain (pA);

        r.expect (uid1.isNotEmpty(),  "first instance instantiated (non-empty uid)");
        r.expect (uid2.isNotEmpty(),  "second instance instantiated (non-empty uid)");
        r.expect (uid1 != uid2,       "the two instances have distinct uids");

        const auto chain = controller.getChain();
        r.expect (chain.size() == 2,  "chain holds exactly two plugins");
        if (chain.size() == 2)
        {
            r.expect (! chain[0].missing && ! chain[1].missing, "both instances are live (not missing)");
            r.expect (chain[0].name == pA.name && chain[1].name == pA.name, "both instances name the same plugin");
        }

        auto* n1 = controller.getNodeForUid (uid1);
        auto* n2 = controller.getNodeForUid (uid2);
        r.expect (n1 != nullptr && n2 != nullptr && n1 != n2, "both map to distinct live graph nodes");

        controller.save (*settings);
        f.deleteFile();
    }

    //==========================================================================
    // Test B — bypass survives a save() -> fresh loadFrom() restart cycle.
    //==========================================================================
    std::cout << "\n[B] bypass survives restart" << std::endl;
    {
        const File f = tempSettingsFile ("bypass");
        f.deleteFile();

        String firstUid, secondUid;

        {   // Session 1: build a two-plugin chain, bypass the first, save.
            AudioProcessorGraph graph;
            graph.setPlayConfigDetails (2, 2, 44100.0, 512);
            graph.prepareToPlay (44100.0, 512);

            GraphController controller (graph, formatManager);
            auto settings = makeSettings (f);
            controller.loadFrom (*settings);

            firstUid  = controller.appendToChain (pA);
            secondUid = controller.appendToChain (plugins.size() > 1 ? plugins.getReference (1) : pA);

            controller.setBypassed (firstUid, true);

            auto* node = controller.getNodeForUid (firstUid);
            r.expect (node != nullptr && node->isBypassed(),
                      "live node reports bypassed immediately (pass-through, no rebuild)");

            controller.save (*settings);
            settings->saveIfNeeded();
        }

        {   // Confirm the persisted file actually carries a graphDocument.
            auto settings = makeSettings (f);
            r.expect (settings->getValue ("graphDocument").isNotEmpty(),
                      "graphDocument written to settings on save");
        }

        {   // Session 2: brand-new graph + controller, load the same file.
            AudioProcessorGraph graph;
            graph.setPlayConfigDetails (2, 2, 44100.0, 512);
            graph.prepareToPlay (44100.0, 512);

            GraphController controller (graph, formatManager);
            auto settings = makeSettings (f);
            controller.loadFrom (*settings);

            const auto chain = controller.getChain();
            r.expect (chain.size() == 2, "reloaded chain still has two plugins");

            if (chain.size() == 2)
            {
                r.expect (chain[0].uid == firstUid && chain[1].uid == secondUid,
                          "node uids are stable across restart");
                r.expect (chain[0].bypassed,   "first plugin is still bypassed after restart");
                r.expect (! chain[1].bypassed, "second plugin is still enabled after restart");

                auto* node = controller.getNodeForUid (firstUid);
                r.expect (node != nullptr && node->isBypassed(),
                          "live node re-applies the bypass on rebuild");
            }
        }

        f.deleteFile();
    }

    //==========================================================================
    // Test C — pre-2026 legacy settings migrate on first load, then persist.
    //==========================================================================
    std::cout << "\n[C] legacy first-run migration" << std::endl;
    {
        const File f = tempSettingsFile ("legacy");
        f.deleteFile();

        {   // Seed a legacy file: one plugin, bypassed, NO graphDocument.
            auto settings = makeSettings (f);
            writeLegacySettings (*settings, { pA }, { true });
            r.expect (settings->getValue ("graphDocument").isEmpty(),
                      "seed file has no graphDocument (genuine pre-2026 layout)");
            r.expect (settings->getValue ("pluginListActive").isNotEmpty(),
                      "seed file has legacy pluginListActive");
        }

        {   // First load with the new build -> migration path.
            AudioProcessorGraph graph;
            graph.setPlayConfigDetails (2, 2, 44100.0, 512);
            graph.prepareToPlay (44100.0, 512);

            GraphController controller (graph, formatManager);
            auto settings = makeSettings (f);
            controller.loadFrom (*settings);

            const auto chain = controller.getChain();
            r.expect (chain.size() == 1, "migrated chain has the one legacy plugin");
            if (chain.size() == 1)
            {
                r.expect (chain[0].name == pA.name, "migrated plugin name matches");
                r.expect (chain[0].bypassed,        "migrated bypass flag preserved");
                r.expect (! chain[0].missing,       "migrated plugin instantiated live");
            }

            controller.save (*settings);
            settings->saveIfNeeded();
        }

        {   // Legacy keys must remain (downgrade-safe) AND a graphDocument now exists.
            auto settings = makeSettings (f);
            r.expect (settings->getValue ("graphDocument").isNotEmpty(),
                      "graphDocument persisted after migration");
            r.expect (settings->getValue ("pluginListActive").isNotEmpty(),
                      "legacy keys left intact (downgrade-safe)");
        }

        f.deleteFile();
    }

    //==========================================================================
    // Test D — the master meters track real audio flowing through the graph.
    //          Proves the spliced MeterProcessors measure a live signal (not just
    //          a synthetic unit test) and fall back to zero on silence.
    //==========================================================================
    std::cout << "\n[D] master meters track live audio" << std::endl;
    {
        const double sr        = 44100.0;
        const int    blockSize = 512;

        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, sr, blockSize);
        graph.prepareToPlay (sr, blockSize);

        GraphController controller (graph, formatManager);
        const File f = tempSettingsFile ("meter");
        f.deleteFile();
        auto settings = makeSettings (f);
        controller.loadFrom (*settings);

        AudioBuffer<float> block (2, blockSize);
        MidiBuffer midi;
        Random rng (0x1965abcd);

        auto driveNoise = [&] (const MeterTap* meter, int count)
        {
            return driveNoiseBlocks (graph, block, midi, rng, [meter] { return meter; }, count);
        };

        // Process `count` silent blocks and return the meter's final ch0 peak.
        auto driveSilence = [&] (const MeterTap* meter, int count)
        {
            block.clear();
            for (int b = 0; b < count; ++b) { midi.clear(); graph.processBlock (block, midi); }
            return meter != nullptr ? meter->read().peak[0] : 1.0f;
        };

        // Phase 1 — with a real plugin in the chain, noise must register on both
        // the input (dry) and output (post-plugin) master meters. Proof the taps
        // see live audio flowing through a genuine plugin node.
        const String uid = controller.appendToChain (pA);
        r.expect (uid.isNotEmpty(), "meter-test plugin instantiated");

        const MeterTap* inMeter  = controller.getInputMeter();
        const MeterTap* outMeter = controller.getOutputMeter();
        r.expect (inMeter  != nullptr, "input meter present after rebuild");
        r.expect (outMeter != nullptr, "output meter present after rebuild");

        const float inNoise  = driveNoise (inMeter,  32);
        const float outNoise = driveNoise (outMeter, 32);
        r.expect (inNoise  > 0.0f, "input meter registers noise through a real plugin chain");
        r.expect (outNoise > 0.0f, "output meter registers signal reaching the output");

        // Phase 2 — decay is a property of the METER, not the plugin: some real
        // plugins idle noisily or self-oscillate (Graillon emits ~+10 dB on a
        // silent input). Remove the plugin so the chain is a clean
        // audioIn -> meters -> audioOut pass-through, then prove the output meter
        // rises on noise and collapses to ~0 on silence.
        controller.removeNode (uid);                // rewire -> fresh meter nodes
        const MeterTap* passOut = controller.getOutputMeter();
        r.expect (passOut != nullptr, "output meter present after plugin removal");

        const float passNoise = driveNoise   (passOut, 8);
        const float passRest  = driveSilence (passOut, 8);
        r.expect (passNoise > 0.0f,    "pass-through output meter rises on noise");
        r.expect (passRest  < 1.0e-4f, "pass-through output meter falls back to ~0 on silence");

        controller.save (*settings);
        f.deleteFile();
    }

    //==========================================================================
    // Test E — per-node channel strip: output gain scales the node meter and
    //          hard pan mutes the opposite channel. The plugin is BYPASSED so the
    //          strip sees a deterministic pass-through of the injected noise —
    //          this isolates the strip's gain/pan DSP from plugin nondeterminism
    //          (some real FX self-oscillate, cf. Test D). The strip is spliced
    //          after the node regardless of the bypass flag.
    //==========================================================================
    std::cout << "\n[E] per-node channel strip (gain + pan)" << std::endl;
    {
        const double sr        = 44100.0;
        const int    blockSize = 512;

        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, sr, blockSize);
        graph.prepareToPlay (sr, blockSize);

        GraphController controller (graph, formatManager);
        const File f = tempSettingsFile ("strip");
        f.deleteFile();
        auto settings = makeSettings (f);
        controller.loadFrom (*settings);

        const String uid = controller.appendToChain (pA);
        r.expect (uid.isNotEmpty(), "strip-test plugin instantiated");

        controller.setBypassed (uid, true);   // deterministic pass-through into the strip

        const MeterTap* nodeMeter = controller.getNodeMeter (uid);
        r.expect (nodeMeter != nullptr, "per-node meter present for the plugin node");

        Random rng (0x57a1b3);
        AudioBuffer<float> block (2, blockSize);
        MidiBuffer midi;

        // Drive `count` blocks of white noise; report the max L/R peaks the node
        // meter saw over that phase (resets each call).
        auto drive = [&] (int count, float& maxL, float& maxR)
        {
            maxL = maxR = 0.0f;
            for (int b = 0; b < count; ++b)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    float* d = block.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        d[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.5f;
                }
                midi.clear();
                graph.processBlock (block, midi);
                if (nodeMeter != nullptr)
                {
                    const auto lv = nodeMeter->read();
                    maxL = jmax (maxL, lv.peak[0]);
                    maxR = jmax (maxR, lv.peak[1]);
                }
            }
        };

        float uL, uR, hL, hR, pL, pR;

        controller.setNodeGain (uid, 1.0f);
        drive (40, uL, uR);                       // fills through the bypass latency
        const float unityPeak = jmax (uL, uR);
        r.expect (unityPeak > 0.0f, "node meter registers signal at unity gain");

        controller.setNodeGain (uid, 0.5f);
        r.expect (std::abs (controller.getNodeGain (uid) - 0.5f) < 1.0e-6f, "getNodeGain reflects the set value");
        drive (40, hL, hR);
        const float halfPeak = jmax (hL, hR);
        r.expect (halfPeak > 0.0f && halfPeak < unityPeak * 0.7f, "node meter drops with lower gain (0.5)");

        controller.setNodePan (uid, -1.0f);
        r.expect (std::abs (controller.getNodePan (uid) + 1.0f) < 1.0e-6f, "getNodePan reflects the set value");
        drive (40, pL, pR);
        r.expect (pR < 1.0e-4f, "hard-left pan mutes the right meter channel");
        r.expect (pL > 0.0f,    "hard-left pan keeps the left meter channel alive");

        controller.save (*settings);
        f.deleteFile();
    }

    //==========================================================================
    // Test F — plugin-parameter bridge: driving a hosted plugin's OWN parameter
    //          via setNodeParameter moves getNodeParameter and the plugin's
    //          reported value text. Uses the live instance's getParameters() list
    //          (host-side setValueNotifyingHost), not the host gain/pan strip.
    //          SKIPs green if the discovered plugin exposes no usable parameter.
    //==========================================================================
    std::cout << "\n[F] plugin-parameter bridge" << std::endl;
    {
        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, 44100.0, 512);
        graph.prepareToPlay (44100.0, 512);

        GraphController controller (graph, formatManager);
        const File f = tempSettingsFile ("params");
        f.deleteFile();
        auto settings = makeSettings (f);
        controller.loadFrom (*settings);

        const String uid = controller.appendToChain (pA);
        r.expect (uid.isNotEmpty(), "param-test plugin instantiated");

        auto ps = controller.getNodeParameters (uid, /*automatableOnly*/ true);
        if (ps.empty())
            ps = controller.getNodeParameters (uid, false);      // fall back to any param
        r.expect (! ps.empty(), "hosted plugin exposes at least one parameter");

        // The plugin's current text for parameter `idx` (fresh snapshot).
        auto textAt = [&] (int idx) -> String
        {
            for (const auto& info : controller.getNodeParameters (uid, false))
                if (info.index == idx)
                    return info.text;
            return {};
        };

        // Find the first parameter whose normalized value actually responds to a
        // 0.0 -> 0.9 sweep (skips fixed/meter-style read-only params); prefer one
        // whose reported text moves too so we can assert both.
        int  valueMover = -1, textMover = -1;
        for (const auto& info : ps)
        {
            controller.setNodeParameter (uid, info.index, 0.0f);
            const float v0 = controller.getNodeParameter (uid, info.index);
            const String t0 = textAt (info.index);

            controller.setNodeParameter (uid, info.index, 0.9f);
            const float v1 = controller.getNodeParameter (uid, info.index);
            const String t1 = textAt (info.index);

            if (v1 - v0 > 0.1f)
            {
                if (valueMover < 0) valueMover = info.index;
                if (textMover  < 0 && t1 != t0) { textMover = info.index; break; }
            }
        }

        if (textMover < 0 && valueMover < 0)
        {
            std::cout << "  SKIP no automatable parameter responded to a 0->0.9 sweep on this plugin" << std::endl;
        }
        else
        {
            const int idx = textMover >= 0 ? textMover : valueMover;

            controller.setNodeParameter (uid, idx, 0.0f);
            const float low   = controller.getNodeParameter (uid, idx);
            const String lowT = textAt (idx);

            controller.setNodeParameter (uid, idx, 0.9f);
            const float high   = controller.getNodeParameter (uid, idx);
            const String highT = textAt (idx);

            r.expect (high - low > 0.1f,
                      "setNodeParameter moves getNodeParameter (0.0 -> 0.9)");

            if (textMover >= 0)
                r.expect (highT != lowT, "plugin's reported value text tracks the parameter");
            else
                std::cout << "  note  chosen param reports no text change; value move verified" << std::endl;
        }

        controller.save (*settings);
        f.deleteFile();
    }

    //==========================================================================
    // Test G — parallel-branch routing with live plugins: rewire a serial
    //          in -> A -> B -> out chain into the fan-out/fan-in
    //          in -> A -> out  ||  in -> B -> out via connect/disconnect (which
    //          rewire WITHOUT reloading plugins), reject cycle-forming edges,
    //          and prove with white noise that both branches carry signal — then
    //          that disconnecting one branch silences only that branch. Both
    //          plugins are BYPASSED for the metering phases (cf. Tests D/E:
    //          some real FX self-oscillate, which would fake a live branch).
    //==========================================================================
    std::cout << "\n[G] parallel-branch routing" << std::endl;
    {
        const double sr        = 44100.0;
        const int    blockSize = 512;

        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, sr, blockSize);
        graph.prepareToPlay (sr, blockSize);

        GraphController controller (graph, formatManager);
        const File f = tempSettingsFile ("routing");
        f.deleteFile();
        auto settings = makeSettings (f);
        controller.loadFrom (*settings);

        const PluginDescription& pB = plugins.size() > 1 ? plugins.getReference (1) : pA;

        const String uidA = controller.appendToChain (pA);
        const String uidB = controller.appendToChain (pB);
        r.expect (uidA.isNotEmpty() && uidB.isNotEmpty(), "both branch plugins instantiated");
        r.expect (controller.isLinearChain(), "serial in->A->B->out starts as a linear chain");

        const String inUid  = controller.document.getIoNodeUid (true);
        const String outUid = controller.document.getIoNodeUid (false);
        r.expect (inUid.isNotEmpty() && outUid.isNotEmpty(), "document exposes the IO node uids");

        // Rejections while still serial: B -> A would close the cycle A -> B -> A,
        // and audioOut can never be a source.
        r.expect (! controller.canConnect (uidB, 0, uidA, 0),
                  "canConnect rejects a cycle-forming edge (B -> A while A -> B exists)");
        r.expect (! controller.connect (outUid, 0, uidA, 0),
                  "connect rejects an edge out of audioOut (out -> A)");

        // Rewire serial -> parallel: cut A -> B, then add in -> B and A -> out.
        bool branched = true;
        for (int ch = 0; ch < 2; ++ch)
        {
            controller.disconnect (uidA, ch, uidB, ch);
            branched = controller.connect (inUid, ch, uidB, ch)  && branched;
            branched = controller.connect (uidA,  ch, outUid, ch) && branched;
        }
        r.expect (branched, "connect accepts both branch edges (in -> B, A -> out)");
        r.expect (! controller.isLinearChain(), "parallel graph is no longer a linear chain");

        // Deterministic pass-through into the node strips for the meter phases.
        controller.setBypassed (uidA, true);
        controller.setBypassed (uidB, true);

        AudioBuffer<float> block (2, blockSize);
        MidiBuffer midi;
        Random rng (0x6e0d5a11);

        // Drive `count` white-noise blocks; report each meter's max ch0 peak over
        // the phase, and B's FINAL reading (to prove decay, not just a lull).
        // Meter pointers are re-fetched each call: connect/disconnect re-splice
        // the runtime strip/meter nodes, invalidating earlier pointers.
        float maxA = 0.0f, maxB = 0.0f, maxOut = 0.0f, lastB = 0.0f;
        auto driveNoise = [&] (int count)
        {
            const MeterTap* mA   = controller.getNodeMeter (uidA);
            const MeterTap* mB   = controller.getNodeMeter (uidB);
            const MeterTap* mOut = controller.getOutputMeter();
            maxA = maxB = maxOut = lastB = 0.0f;
            driveNoiseBlocks (graph, block, midi, rng, [] { return nullptr; }, count, [&]
            {
                if (mA   != nullptr) maxA   = jmax (maxA,   mA->read().peak[0]);
                if (mB   != nullptr) { lastB = mB->read().peak[0]; maxB = jmax (maxB, lastB); }
                if (mOut != nullptr) maxOut = jmax (maxOut, mOut->read().peak[0]);
            });
            return controller.getNodeMeter (uidA) != nullptr
                && controller.getNodeMeter (uidB) != nullptr
                && controller.getOutputMeter()    != nullptr;
        };

        r.expect (driveNoise (40), "node + master meters present on the parallel graph");
        r.expect (maxA   > 0.0f, "branch A meter registers noise");
        r.expect (maxB   > 0.0f, "branch B meter registers noise");
        r.expect (maxOut > 0.0f, "master OUT meter registers the fan-in");

        // Cut branch B at its input; A must keep flowing while B falls silent.
        for (int ch = 0; ch < 2; ++ch)
            controller.disconnect (inUid, ch, uidB, ch);

        r.expect (driveNoise (40), "meters present again after the branch disconnect");
        r.expect (maxA   > 0.0f,    "branch A meter still registers after cutting B");
        r.expect (maxOut > 0.0f,    "master OUT meter still registers after cutting B");
        r.expect (lastB  < 1.0e-4f, "disconnected branch B meter falls to ~0");

        controller.save (*settings);
        f.deleteFile();
    }

    //==========================================================================
    // Test H — MIDI routing (M7): if a discovered plugin acceptsMidi(), prove the
    //          controller splices a runtime midiInputNode edge INTO it on
    //          midiChannelIndex while receivesMidi is set, and DROPS that edge when
    //          the flag is cleared (live re-splice, no plugin reload) — then
    //          restores it. The plugin scan filters instruments, so most stereo FX
    //          do not accept MIDI: if none do, SKIP the routing proof but still
    //          assert the per-node receivesMidi flag toggles and survives save/reload.
    //==========================================================================
    std::cout << "\n[H] MIDI routing" << std::endl;
    {
        const double sr        = 44100.0;
        const int    blockSize = 512;

        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, sr, blockSize);
        graph.prepareToPlay (sr, blockSize);

        GraphController controller (graph, formatManager);
        const File f = tempSettingsFile ("midi");
        f.deleteFile();
        auto settings = makeSettings (f);
        controller.loadFrom (*settings);

        // Count live-graph edges landing on `target` at the MIDI channel index.
        auto midiEdgesInto = [&graph] (AudioProcessorGraph::NodeID target)
        {
            int n = 0;
            for (const auto& c : graph.getConnections())
                if (c.destination.nodeID == target
                    && c.destination.channelIndex == AudioProcessorGraph::midiChannelIndex)
                    ++n;
            return n;
        };

        // Find a discovered plugin that accepts MIDI; drop any that do not.
        String midiUid;
        for (const auto& d : plugins)
        {
            const String uid = controller.appendToChain (d);
            if (uid.isEmpty())
                continue;
            if (controller.nodeAcceptsMidi (uid)) { midiUid = uid; break; }
            controller.removeNode (uid);
        }

        if (midiUid.isNotEmpty())
        {
            r.expect (! controller.getNodeReceivesMidi (midiUid),
                      "a newly added effect starts with receivesMidi off (v1.2)");
            controller.setNodeReceivesMidi (midiUid, true);   // the splice assertions below need it on
            r.expect (controller.nodeAcceptsMidi (midiUid),
                      "nodeAcceptsMidi reports true for the discovered plugin");

            auto* node = controller.getNodeForUid (midiUid);
            r.expect (node != nullptr, "live node present for the MIDI plugin");

            if (node != nullptr)
            {
                r.expect (midiEdgesInto (node->nodeID) == 1,
                          "midiInputNode is spliced to the plugin on midiChannelIndex");

                // Drive a note through the graph: must process cleanly with MIDI
                // routed (we assert structure, not the plugin's own response).
                AudioBuffer<float> block (2, blockSize); block.clear();
                MidiBuffer midi;
                midi.addEvent (MidiMessage::noteOn  (1, 60, (uint8) 100), 0);
                midi.addEvent (MidiMessage::noteOff (1, 60),               blockSize / 2);
                graph.processBlock (block, midi);
                r.expect (true, "note-on/off block processed with MIDI routed");

                // Clear the flag: the MIDI edge must drop (plugin instance kept).
                controller.setNodeReceivesMidi (midiUid, false);
                node = controller.getNodeForUid (midiUid);   // re-fetch after re-splice
                r.expect (node != nullptr && midiEdgesInto (node->nodeID) == 0,
                          "clearing receivesMidi drops the MIDI edge");

                // Restore: the edge comes back.
                controller.setNodeReceivesMidi (midiUid, true);
                node = controller.getNodeForUid (midiUid);
                r.expect (node != nullptr && midiEdgesInto (node->nodeID) == 1,
                          "restoring receivesMidi re-splices the MIDI edge");
            }
        }
        else
        {
            std::cout << "  SKIP no discovered plugin accepts MIDI; "
                         "verifying the receivesMidi flag toggles + persists only" << std::endl;

            const String uid = controller.appendToChain (pA);
            r.expect (uid.isNotEmpty(), "plugin instantiated for the flag round-trip");
            r.expect (! controller.getNodeReceivesMidi (uid), "a newly added effect starts with receivesMidi off (v1.2)");
            controller.setNodeReceivesMidi (uid, true);
            r.expect (controller.getNodeReceivesMidi (uid), "receivesMidi can be set");
            controller.setNodeReceivesMidi (uid, false);
            r.expect (! controller.getNodeReceivesMidi (uid), "receivesMidi can be cleared");
            controller.save (*settings);

            // Reload into a fresh controller/graph and confirm the flag persisted.
            AudioProcessorGraph graph2;
            graph2.setPlayConfigDetails (2, 2, sr, blockSize);
            graph2.prepareToPlay (sr, blockSize);
            GraphController c2 (graph2, formatManager);
            c2.loadFrom (*settings);
            r.expect (! c2.getNodeReceivesMidi (uid), "cleared receivesMidi survives save + reload");
        }

        controller.save (*settings);
        f.deleteFile();
    }

    //==========================================================================
    // Test I — presets round-trip. Build two distinct chains, snapshot each into a
    //          PresetStore, round-trip the store through XML (as persisted in
    //          settings), then switch between presets and assert the controller's
    //          document rebuilds to match each. Also assert names + active index
    //          survive a real settings save/reload.
    //==========================================================================
    std::cout << "\n[I] presets round-trip" << std::endl;
    {
        const double sr        = 44100.0;
        const int    blockSize = 512;

        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, sr, blockSize);
        graph.prepareToPlay (sr, blockSize);

        GraphController controller (graph, formatManager);
        const File f = tempSettingsFile ("presets");
        f.deleteFile();
        auto settings = makeSettings (f);
        controller.loadFrom (*settings);

        const PluginDescription& p2 = plugins.size() > 1 ? plugins.getReference (1) : pA;

        // Preset A: a single-plugin chain.
        const String a1 = controller.appendToChain (pA);
        r.expect (a1.isNotEmpty(), "preset-A plugin instantiated");

        PresetStore store;
        const int idxA = store.addPreset ("A", controller.snapshotDocument());
        r.expect (idxA == 0, "first preset added at index 0");

        // Preset B: a two-plugin chain (a distinct document).
        const String b2 = controller.appendToChain (p2);
        r.expect (b2.isNotEmpty(), "preset-B second plugin instantiated");
        const int idxB = store.addPreset ("B", controller.snapshotDocument());
        r.expect (store.getNumPresets() == 2, "store holds two presets");

        // Round-trip the whole store through XML (as persisted under "presets").
        store.setActiveIndex (idxB);
        PresetStore round = PresetStore::fromXml (store.toXml());
        r.expect (round.isValid(), "store XML round-trips to a valid store");
        r.expect (round.getNumPresets() == 2, "preset count survives round-trip");
        r.expect (round.getPresetName (0) == "A" && round.getPresetName (1) == "B",
                  "preset names survive round-trip");
        r.expect (round.getActiveIndex() == idxB, "active index survives round-trip");

        // Switch between presets and assert the controller document matches each.
        controller.loadDocument (round.getPresetDocument (idxA));
        r.expect (controller.isLinearChain() && (int) controller.getChain().size() == 1,
                  "loading preset A rebuilds the 1-plugin chain");
        controller.loadDocument (round.getPresetDocument (idxB));
        r.expect (controller.isLinearChain() && (int) controller.getChain().size() == 2,
                  "loading preset B rebuilds the 2-plugin chain");

        // Persist through a real settings save/reload too.
        settings->setValue ("presets", store.toXml());
        settings->saveIfNeeded();
        PresetStore fromSettings = PresetStore::fromXml (settings->getValue ("presets"));
        r.expect (fromSettings.getNumPresets() == 2 && fromSettings.getActiveIndex() == idxB,
                  "presets survive a settings save/reload");

        f.deleteFile();
    }

    //==========================================================================
    // Test J — node canvas positions are user-owned and survive chain edits
    // (append / remove) that rebuild the graph. Guards the Cowork fix that stopped
    // rewriteChainConnections / appendToChain from rewriting node x/y.
    //==========================================================================
    std::cout << "\n[J] node positions survive chain edits" << std::endl;
    {
        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, 44100.0, 512);
        graph.prepareToPlay (44100.0, 512);

        GraphController controller (graph, formatManager);
        const File f = tempSettingsFile ("positions");
        f.deleteFile();
        auto settings = makeSettings (f);
        controller.loadFrom (*settings);

        const String uid1 = controller.appendToChain (pA);
        const String uid2 = controller.appendToChain (pA);   // two instances of pA is fine
        r.expect (uid1.isNotEmpty() && uid2.isNotEmpty(), "two plugins instantiated");

        // Distinct, non-default positions (px; the fallback for a missing position is (400, 260)).
        controller.setNodePosition (uid1, 300.0f, 200.0f);
        controller.setNodePosition (uid2, 900.0f, 500.0f);

        // A structural edit: add a third node then remove it.
        const String uid3 = controller.appendToChain (pA);
        r.expect (uid3.isNotEmpty(), "third plugin instantiated");
        controller.removeNode (uid3);

        const auto eq = [] (float a, float b) { return std::abs (a - b) < 1.0e-6f; };
        r.expect (eq (controller.getNodeX (uid1), 300.0f) && eq (controller.getNodeY (uid1), 200.0f),
                  "node 1 position unchanged after append + remove");
        r.expect (eq (controller.getNodeX (uid2), 900.0f) && eq (controller.getNodeY (uid2), 500.0f),
                  "node 2 position unchanged after append + remove");

        controller.save (*settings);
        f.deleteFile();
    }

    //==========================================================================
    // Test K — a plugin that FAILS TO LOAD runs as a pass-through: the chain is
    // NOT severed. Fabricate an un-loadable description (a real plugin's fields
    // pointed at a nonexistent file), append it, and prove the node is reported
    // missing yet white noise still reaches the output meter (audio flows through
    // the missing slot) and decays to ~0 on silence.
    //==========================================================================
    std::cout << "\n[K] missing plugin passes audio through" << std::endl;
    {
        const double sr        = 44100.0;
        const int    blockSize = 512;

        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, sr, blockSize);
        graph.prepareToPlay (sr, blockSize);

        GraphController controller (graph, formatManager);
        const File f = tempSettingsFile ("missing");
        f.deleteFile();
        auto settings = makeSettings (f);
        controller.loadFrom (*settings);

        // An intentionally un-loadable plugin: pA's fields, but the file it would
        // load from does not exist, so createPluginInstance must fail.
        PluginDescription bogus = pA;
        bogus.name             = "DeliberatelyMissing";
        bogus.fileOrIdentifier = File::getSpecialLocation (File::tempDirectory)
                                     .getChildFile ("lighthost_no_such_plugin_zzz.vst3")
                                     .getFullPathName();

        const String uid = controller.appendToChain (bogus);
        r.expect (uid.isNotEmpty(), "missing plugin still produces a live node (pass-through stand-in)");

        const auto chain = controller.getChain();
        r.expect (chain.size() == 1, "chain holds the one (missing) plugin slot");
        if (chain.size() == 1)
            r.expect (chain[0].missing, "the un-loadable plugin is reported missing");

        AudioBuffer<float> block (2, blockSize);
        MidiBuffer midi;
        Random rng (0x0badf00d);

        const MeterTap* outMeter = controller.getOutputMeter();
        r.expect (outMeter != nullptr, "output meter present with a missing plugin in the chain");

        auto driveNoise = [&] (int count)
        {
            return driveNoiseBlocks (graph, block, midi, rng, [outMeter] { return outMeter; }, count);
        };

        auto driveSilence = [&] (int count)
        {
            block.clear();
            for (int b = 0; b < count; ++b) { midi.clear(); graph.processBlock (block, midi); }
            return outMeter != nullptr ? outMeter->read().peak[0] : 1.0f;
        };

        r.expect (driveNoise   (16) > 0.0f,    "noise reaches the output through the missing slot (chain not severed)");
        r.expect (driveSilence (16) < 1.0e-4f, "output falls back to ~0 on silence (no injected signal)");

        controller.save (*settings);
        f.deleteFile();
    }

    //==========================================================================
    // Test M — per-node dry/wet mix (ported onto the batched engine 2026-10-20).
    //          The plugin is BYPASSED, so wet == dry == the injected noise; the
    //          node's post-mix OUT meter must read the SAME non-zero level at mix=1
    //          (pure wet) and mix=0 (pure dry). If the dry branch were mis-wired or
    //          silent, mix=0 would collapse to ~0 while mix=1 stayed up. The blend
    //          MATH is proven headless in MixTests; this proves the graph wiring.
    //          Also: crossing 100 % builds / tears down the branch with exactly one
    //          publish each way, changes below 100 % publish nothing, the level
    //          returns to wet, removeNode leaves no orphaned mix node, and mix
    //          persists across save -> reload (which rebuilds the branch).
    //==========================================================================
    std::cout << "\n[M] per-node dry/wet mix" << std::endl;
    {
        const double sr        = 44100.0;
        const int    blockSize = 512;
        GraphFixture fx (formatManager, "mix", sr, blockSize);
        GraphController& controller = *fx.controller;
        AudioProcessorGraph& graph = fx.graph;

        const auto countMixNodes = [] (AudioProcessorGraph& g)
        {
            int n = 0;
            for (auto* node : g.getNodes())
                if (dynamic_cast<MixProcessor*> (node->getProcessor()) != nullptr)
                    ++n;
            return n;
        };
        const auto publishesDuring = [&controller] (const std::function<void()>& gesture)
        {
            const int before = controller.getPublishCount();
            gesture();
            return controller.getPublishCount() - before;
        };

        const String uid = controller.appendToChain (pA);
        r.expect (uid.isNotEmpty(), "mix-test plugin instantiated");
        r.expect (std::abs (controller.getNodeMix (uid) - 1.0f) < 1.0e-6f, "default mix is fully wet");
        r.expect (countMixNodes (graph) == 0, "a fully wet graph has no mix node (plain topology)");

        controller.setBypassed (uid, true);   // wet == dry == input noise (deterministic)

        Random rng (0x3d1cef);
        AudioBuffer<float> block (2, blockSize);
        MidiBuffer midi;
        // Node OUT meter (post-mix), re-fetched per block: a rewire re-splices the strip.
        const auto drive = [&] (int count)
        {
            return driveNoiseBlocks (graph, block, midi, rng, [&controller, &uid] { return controller.getNodeMeter (uid); }, count);
        };
        const auto sameLevel = [] (float a, float b) { return std::abs (a - b) < 0.15f * jmax (a, b, 1.0e-6f); };

        const float wetPeak = drive (40);
        r.expect (wetPeak > 0.0f, "node meter registers signal at mix=1 (fully wet)");

        // First drop below 100 %: builds the dry branch, one publish.
        r.expect (publishesDuring ([&] { controller.setNodeMix (uid, 0.0f); }) == 1,
                  "first drop below 100% publishes once (branch built)");
        r.expect (countMixNodes (graph) == 1, "mix < 1 splices exactly one mix node");
        r.expect (std::abs (controller.getNodeMix (uid)) < 1.0e-6f, "getNodeMix reflects the fully-dry setting");
        const float dryPeak = drive (40);
        r.expect (dryPeak > 0.0f, "node meter still registers signal at mix=0 (dry branch carries the input)");
        r.expect (sameLevel (dryPeak, wetPeak), "with the plugin bypassed, dry and wet blends read the same level");

        // Below 100 %: live via the atomic, no topology change.
        r.expect (publishesDuring ([&] { controller.setNodeMix (uid, 0.5f); }) == 0,
                  "mix changes below 100% publish nothing (live atomic)");
        r.expect (std::abs (controller.getNodeMix (uid) - 0.5f) < 1.0e-6f, "getNodeMix reflects the half setting");
        r.expect (drive (40) > 0.0f, "node meter registers signal at mix=0.5");

        // Back to 100 %: branch torn down (one publish), level returns to the wet level.
        r.expect (publishesDuring ([&] { controller.setNodeMix (uid, 1.0f); }) == 1,
                  "returning to 100% publishes once (branch torn down)");
        r.expect (countMixNodes (graph) == 0, "fully wet again: no mix node left");
        r.expect (sameLevel (drive (40), wetPeak), "level after return-to-wet matches the original wet level");

        // Persistence: mix survives save() -> fresh loadFrom(), and the reload rebuilds the branch.
        controller.setNodeMix (uid, 0.5f);
        controller.save (*fx.settings);
        {
            AudioProcessorGraph g2;
            g2.setPlayConfigDetails (2, 2, sr, blockSize);
            g2.prepareToPlay (sr, blockSize);
            GraphController c2 (g2, formatManager);
            c2.loadFrom (*fx.settings);
            r.expect (std::abs (c2.getNodeMix (uid) - 0.5f) < 1.0e-6f, "mix survives save() -> fresh loadFrom()");
            r.expect (countMixNodes (g2) == 1, "reload with mix < 1 rebuilds the dry branch");
        }

        // Teardown mirror: deleting a node with a live branch leaves no orphaned mix node.
        controller.removeNode (uid);
        r.expect (countMixNodes (graph) == 0, "removeNode leaves no orphaned mix node");
    }

    //==========================================================================
    // Test N — removeNode heals around the deleted node WITHOUT rewriting the
    //          rest of the graph ("M" is reserved by the dry/wet test on the
    //          feature/dry-wet-mix branch). Build in -> P1 -> P2 -> out plus a
    //          parallel side edge P1 -> out (branched, non-linear). Deleting P2
    //          must bridge P1 -> out WITHOUT duplicating the pre-existing side
    //          edge, leave nothing referencing P2, and keep audio flowing.
    //          Then deleting P1 from the now-linear chain must heal in -> out.
    //==========================================================================
    std::cout << "\n[N] removeNode heals a branched graph" << std::endl;
    {
        const double sr        = 44100.0;
        const int    blockSize = 512;

        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, sr, blockSize);
        graph.prepareToPlay (sr, blockSize);

        GraphController controller (graph, formatManager);
        const File f = tempSettingsFile ("removeheal");
        f.deleteFile();
        auto settings = makeSettings (f);
        controller.loadFrom (*settings);

        const String p1 = controller.appendToChain (pA);
        const String p2 = controller.appendToChain (pA);
        const String out = controller.document.getIoNodeUid (false);
        const String in  = controller.document.getIoNodeUid (true);
        r.expect (p1.isNotEmpty() && p2.isNotEmpty(), "two plugin nodes instantiated");

        // Branch: parallel side edge P1 -> out alongside the serial P1 -> P2 -> out.
        r.expect (controller.connect (p1, 0, out, 0), "side edge P1->out ch0 connects");
        r.expect (controller.connect (p1, 1, out, 1), "side edge P1->out ch1 connects");
        r.expect (! controller.isLinearChain(), "graph is branched (non-linear) before the delete");

        // The delete must be incremental: the OTHER plugin's live instance (and by
        // extension its editor) survives the edit — no reload, no teardown.
        const AudioProcessor* p1InstanceBefore = controller.getNodeForUid (p1)->getProcessor();

        controller.removeNode (p2);
        r.expect (! controller.document.getNodeByUid (p2).isValid(), "deleted node left the document");
        r.expect (controller.getNodeForUid (p1) != nullptr
               && controller.getNodeForUid (p1)->getProcessor() == p1InstanceBefore,
                  "the surviving plugin kept its live instance (incremental delete, no reload)");

        // Healing bridges P1 -> out, which is the SAME edge as the pre-existing
        // branch — it must survive exactly once per channel (deduped, untouched).
        int p1OutCh0 = 0, p1OutCh1 = 0, referencesP2 = 0;
        for (const auto& c : controller.getConnections())
        {
            if (c.srcUid == p2 || c.dstUid == p2) ++referencesP2;
            if (c.srcUid == p1 && c.dstUid == out && c.srcCh == 0 && c.dstCh == 0) ++p1OutCh0;
            if (c.srcUid == p1 && c.dstUid == out && c.srcCh == 1 && c.dstCh == 1) ++p1OutCh1;
        }
        r.expect (referencesP2 == 0, "no connection still references the deleted node");
        r.expect (p1OutCh0 == 1 && p1OutCh1 == 1, "P1->out survives exactly once per channel (healed, no duplicate)");
        r.expect (controller.isLinearChain(), "in->P1->out is linear again after the delete");

        AudioBuffer<float> block (2, blockSize);
        MidiBuffer midi;
        Random rng (0x11ea1);
        // Meter re-fetched per block: it is re-spliced on every delete/rewire.
        auto driveNoise = [&] (int count)
        {
            return driveNoiseBlocks (graph, block, midi, rng,
                                     [&controller]() -> const MeterTap* { return controller.getOutputMeter(); },
                                     count);
        };
        r.expect (driveNoise (16) > 0.0f, "audio still reaches the output through the healed graph");

        // Branched add: re-branch the graph (side edge in -> out alongside
        // in -> P1 -> out), then add via the shared addPlugin entry point. It must
        // come in UNCONNECTED — every pre-existing edge stays untouched — and still
        // show up in the tray listing (getChain must never omit a live plugin).
        r.expect (controller.connect (in, 0, out, 0) && controller.connect (in, 1, out, 1),
                  "side edge in->out re-branches the graph");
        const auto edgesBefore = controller.getConnections().size();
        const String p3 = controller.addPlugin (pA);
        r.expect (p3.isNotEmpty(), "addPlugin on a branched graph instantiates the node");
        r.expect (controller.getConnections().size() == edgesBefore,
                  "branched add is unconnected: no existing edge was touched");
        {
            bool p1Listed = false, p3Listed = false;
            for (const auto& item : controller.getChain())
            {
                if (item.uid == p1) p1Listed = true;
                if (item.uid == p3) p3Listed = true;
            }
            r.expect (p1Listed && p3Listed, "tray chain listing includes branch and unconnected nodes");
        }
        controller.removeNode (p3);

        // Linear heal: drop the side edge, then deleting the remaining plugin
        // bridges in -> out directly.
        controller.disconnectPair (in, out);
        controller.removeNode (p1);
        r.expect (controller.document.hasConnection (in, 0, out, 0)
               && controller.document.hasConnection (in, 1, out, 1),
                  "deleting the last plugin heals in->out on both channels");
        r.expect (driveNoise (16) > 0.0f, "bare in->out still carries audio after the second delete");

        controller.save (*settings);
        f.deleteFile();
    }

    //==========================================================================
    // Test O — stereo-pair connect primitives (a canvas cable is ch0+ch1 as one
    //          operation: atomic add, duplicate rejection, HALF-pair completion)
    //          and non-stacking placement (consecutive adds never land two cards
    //          on the same spot, even once x saturates at the right edge).
    //==========================================================================
    std::cout << "\n[O] stereo-pair primitives + non-stacking placement" << std::endl;
    {
        GraphFixture fx (formatManager, "pairplacement");
        GraphController& controller = *fx.controller;
        auto& settings = fx.settings;

        const String in  = controller.document.getIoNodeUid (true);
        const String p1  = controller.appendToChain (pA);
        const String p2  = controller.addNodeUnconnected (pA);
        r.expect (p1.isNotEmpty() && p2.isNotEmpty(), "pair-test plugins instantiated");

        // Fresh pair connects atomically (both channels, one call).
        r.expect (controller.canConnectPair (p1, p2), "fresh pair is connectable");
        r.expect (controller.connectPair (p1, p2),    "fresh pair connects");
        r.expect (controller.document.hasConnection (p1, 0, p2, 0)
               && controller.document.hasConnection (p1, 1, p2, 1), "both channels present after connectPair");

        // A full existing pair is a pure duplicate — rejected.
        r.expect (! controller.canConnectPair (p1, p2), "existing full pair is not re-connectable");
        r.expect (! controller.connectPair (p1, p2),    "connectPair rejects a full duplicate");

        // A HALF pair (mono edge, e.g. from a hand-edited document) is repairable:
        // the drag completes the missing channel instead of being stuck rejected.
        controller.disconnect (p1, 1, p2, 1);
        r.expect (controller.canConnectPair (p1, p2), "half pair is connectable (completion)");
        r.expect (controller.connectPair (p1, p2),    "half pair completes");
        r.expect (controller.document.hasConnection (p1, 1, p2, 1), "missing channel was added back");

        // Invalid targets stay invalid pairwise (edge into audioIn).
        r.expect (! controller.canConnectPair (p1, in), "pair into audioIn rejected");

        // disconnectPair drops both channels in one call.
        controller.disconnectPair (p1, p2);
        r.expect (! controller.document.hasConnection (p1, 0, p2, 0)
               && ! controller.document.hasConnection (p1, 1, p2, 1), "disconnectPair removes both channels");

        // A crossed edge (hand-edited/legacy document) is drawn as part of the same cable,
        // so disconnectPair must remove it too.
        controller.document.addConnection (p1, 0, p2, 1);
        controller.document.addConnection (p1, 1, p2, 1);
        controller.disconnectPair (p1, p2);
        int leftover = 0;
        for (const auto& c : controller.getConnections())
            if (c.srcUid == p1 && c.dstUid == p2)
                ++leftover;
        r.expect (leftover == 0, "disconnectPair removes crossed and mono edges between the pair");

        // Placement: six more adds march right, saturate at x=0.9, then cascade
        // down — every plugin node must end up on its own spot.
        for (int i = 0; i < 6; ++i)
            controller.addNodeUnconnected (pA);

        std::vector<std::pair<float, float>> pos;
        ValueTree nodes = controller.document.getNodes();
        for (int i = 0; i < nodes.getNumChildren(); ++i)
        {
            const auto n = nodes.getChild (i);
            if (n.getProperty ("type").toString() == GraphDocument::typePlugin)
                pos.push_back ({ (float) n.getProperty ("x", 0.15f), (float) n.getProperty ("y", 0.5f) });
        }
        r.expect ((int) pos.size() == 8, "eight plugin nodes placed");

        bool anyStacked = false;
        for (size_t a = 0; a < pos.size() && ! anyStacked; ++a)
            for (size_t b = a + 1; b < pos.size() && ! anyStacked; ++b)
                if (std::abs (pos[a].first - pos[b].first) < 0.045f
                 && std::abs (pos[a].second - pos[b].second) < 0.045f)
                    anyStacked = true;
        r.expect (! anyStacked, "no two placed nodes share a spot (anti-stacking cascade)");

        controller.save (*settings);
    }

    // Test O (cont.) — placement reads positions through the document accessors, so a node
    //          that has no x/y property is treated as sitting at the accessor fallback
    //          (400, 260 px) and a new add never lands on top of it.
    {
        GraphFixture fx (formatManager, "placementdefault");
        GraphController& controller = *fx.controller;

        const String a = controller.addNodeUnconnected (pA);
        const String b = controller.addNodeUnconnected (pA);
        r.expect (a.isNotEmpty() && b.isNotEmpty(), "placement-default plugins instantiated");

        // b has no position at all and is therefore at the fallback (400, 260); a sits one slot
        // to its left, so the next add must still keep clear of both.
        controller.setNodePosition (a, 130.0f, 260.0f);
        ValueTree bTree = controller.document.getNodeByUid (b);
        bTree.removeProperty ("x", nullptr);
        bTree.removeProperty ("y", nullptr);

        const String c = controller.addNodeUnconnected (pA);
        r.expect (c.isNotEmpty(), "third plugin instantiated");
        const auto rectOf = [&] (const String& uid)
        { return lighthost::geometry::nodeRect (false, { controller.getNodeX (uid), controller.getNodeY (uid) }); };
        const bool onTopOfB = rectOf (c).intersects (rectOf (b)) || rectOf (c).intersects (rectOf (a));
        r.expect (! onTopOfB, "a new node does not land on a node that has no stored position");

    }

    //==========================================================================
    // Test P — one render-sequence publish per user gesture. Tests N/O render on the
    //          message thread between gestures, so they cannot see intermediate
    //          sequences; the publish counter can. Every graph mutation is batched
    //          (UpdateKind::none), so each gesture must publish exactly once.
    //==========================================================================
    std::cout << "\n[P] exactly one topology publish per gesture" << std::endl;
    {
        GraphFixture fx (formatManager, "publishcount");
        GraphController& controller = *fx.controller;
        auto& settings = fx.settings;

        const auto publishesDuring = [&controller] (const std::function<void()>& gesture)
        {
            const int before = controller.getPublishCount();
            gesture();
            return controller.getPublishCount() - before;
        };

        String p1, p2, p3;
        r.expect (publishesDuring ([&] { p1 = controller.addPlugin (pA); }) == 1, "addPlugin (serial append) publishes once");
        {
            const String outUid = controller.document.getIoNodeUid (false);
            r.expect (controller.getNodeX (outUid) >= controller.getNodeX (p1) + (float) lighthost::geometry::nodeSlot,
                      "addPlugin puts OUT right of the new card (position change publishes nothing extra)");
        }
        p2 = controller.addPlugin (pA);
        p3 = controller.addPlugin (pA);
        r.expect (p1.isNotEmpty() && p2.isNotEmpty() && p3.isNotEmpty(), "publish-test plugins instantiated");

        const String p4 = controller.addNodeUnconnected (pA);
        r.expect (publishesDuring ([&] { controller.connectPair (p3, p4); }) == 1, "connectPair publishes once");
        r.expect (publishesDuring ([&] { controller.disconnectPair (p3, p4); }) == 1, "disconnectPair publishes once");
        r.expect (publishesDuring ([&] { controller.moveUp (p2); }) == 1, "moveUp publishes once");
        r.expect (publishesDuring ([&] { controller.setMonoInput (true); }) == 1, "setMonoInput publishes once");
        r.expect (publishesDuring ([&] { controller.setNodeReceivesMidi (p1, true); }) == 1, "setNodeReceivesMidi publishes once");
        r.expect (publishesDuring ([&] { controller.removeNode (p3); }) == 1, "removeNode publishes once");

        const GraphDocument snapshot = controller.snapshotDocument();
        r.expect (publishesDuring ([&] { controller.loadDocument (snapshot); }) == 1, "loadDocument publishes once");

        controller.save (*settings);
    }

    //==========================================================================
    // Test Q — missing-plugin retry (round-3 finding 6). Edits never re-load a
    //          missing plugin; an explicit retry does. Missing nodes are
    //          fabricated as in Test K (pA pointed at a nonexistent file). A retry
    //          that still fails must change and publish nothing. Then the document
    //          description is "repaired" (as if the plugin came back) and the retry
    //          must swap the stand-in for the real plugin: bypass restored, other
    //          instances untouched, exactly one publish — also for a batch of two.
    //==========================================================================
    std::cout << "\n[Q] missing-plugin retry" << std::endl;
    {
        const int blockSize = 512;
        GraphFixture fx (formatManager, "retrymissing");
        GraphController& controller = *fx.controller;
        auto& settings = fx.settings;
        AudioProcessorGraph& graph = fx.graph;

        PluginDescription bogus = pA;
        bogus.name             = "DeliberatelyMissing";
        bogus.fileOrIdentifier = File::getSpecialLocation (File::tempDirectory)
                                     .getChildFile ("lighthost_no_such_plugin_zzz.vst3")
                                     .getFullPathName();

        const String live = controller.addPlugin (pA);
        const String m1   = controller.addPlugin (bogus);
        const String m2   = controller.addPlugin (bogus);
        const String m3   = controller.addPlugin (bogus);
        r.expect (live.isNotEmpty() && m1.isNotEmpty() && m2.isNotEmpty() && m3.isNotEmpty(),
                  "retry-test nodes created");
        r.expect (controller.getNumMissingPlugins() == 3, "three stand-ins reported missing");

        auto* const liveNode = controller.getNodeForUid (live);
        AudioProcessor* const liveInstance = liveNode != nullptr ? liveNode->getProcessor() : nullptr;
        const auto edgesBefore = controller.getConnections().size();

        // 1. Still unavailable: nothing changes, nothing is published.
        int before = controller.getPublishCount();
        r.expect (! controller.retryMissingPlugin (m1), "retry of a still-missing plugin reports failure");
        r.expect (controller.retryMissingPlugins() == 0, "batch retry recovers nothing while all are still missing");
        r.expect (controller.getPublishCount() == before, "failed retries publish nothing");
        r.expect (controller.getNumMissingPlugins() == 3, "failed retries keep every stand-in");

        // "Repair" a node: give it pA's real description, as if the plugin came back.
        const auto repair = [&controller, &pA] (const String& uid)
        {
            ValueTree node = controller.document.getNodeByUid (uid);
            node.removeChild (node.getChildWithName ("PLUGIN"), nullptr);
            if (auto xml = pA.createXml())
                node.appendChild (ValueTree::fromXml (*xml), nullptr);
        };

        // 2. Single retry: m1 comes back, and its doc bypass flag survives the swap.
        controller.setBypassed (m1, true);
        repair (m1);
        before = controller.getPublishCount();
        r.expect (controller.retryMissingPlugin (m1), "retry succeeds once the plugin is available");
        r.expect (controller.getPublishCount() - before == 1, "a successful retry publishes exactly once");
        r.expect (! controller.isMissing (m1), "recovered node is no longer missing");
        auto* const m1Node = controller.getNodeForUid (m1);
        r.expect (m1Node != nullptr && dynamic_cast<PassThroughProcessor*> (m1Node->getProcessor()) == nullptr,
                  "recovered node runs the real plugin, not the stand-in");
        r.expect (m1Node != nullptr && m1Node->isBypassed(), "recovered node keeps its bypass flag");
        auto* const liveAfter = controller.getNodeForUid (live);
        r.expect (liveAfter != nullptr && liveAfter->getProcessor() == liveInstance,
                  "other plugin instances are untouched by a retry");
        r.expect (controller.getConnections().size() == edgesBefore, "retry keeps the document wiring");

        // 3. Batch retry: two repaired nodes, one publish.
        repair (m2);
        repair (m3);
        before = controller.getPublishCount();
        r.expect (controller.retryMissingPlugins() == 2, "batch retry recovers both repaired plugins");
        r.expect (controller.getPublishCount() - before == 1, "batch retry publishes exactly once");
        r.expect (controller.getNumMissingPlugins() == 0, "no stand-ins left after the batch retry");

        // 4. Audio flows through the recovered chain (all bypassed => deterministic).
        for (const auto& uid : { live, m1, m2, m3 })
            controller.setBypassed (uid, true);
        AudioBuffer<float> block (2, blockSize);
        MidiBuffer midi;
        Random rng (0x5eed0042);
        r.expect (driveNoiseBlocks (graph, block, midi, rng, [&controller] { return controller.getOutputMeter(); }, 16) > 0.0f,
                  "noise reaches the output through the recovered chain");

        controller.save (*settings);
    }

    //==========================================================================
    // Test R — output guard + sanitised values. Exactly one OutputGuardProcessor sits in
    //          the live graph after load and after a rewire (rewire must sweep the old
    //          one). A document whose node gain was corrupted to 1000 loads with the live
    //          strip at <= 2. The publish count per gesture is Test P's job and is
    //          unchanged: the guard is spliced inside the same single rewire publish.
    //==========================================================================
    std::cout << "\n[R] output guard + sanitised persisted values" << std::endl;
    {
        GraphFixture fx (formatManager, "outputguard");
        GraphController& controller = *fx.controller;
        AudioProcessorGraph& graph = fx.graph;

        const auto countGuards = [&graph]
        {
            int n = 0;
            for (auto* node : graph.getNodes())
                if (dynamic_cast<OutputGuardProcessor*> (node->getProcessor()) != nullptr)
                    ++n;
            return n;
        };

        r.expect (countGuards() == 1, "exactly one OutputGuardProcessor after load");

        const String uid = controller.addPlugin (pA);
        r.expect (uid.isNotEmpty(), "guard-test plugin instantiated");
        r.expect (countGuards() == 1, "exactly one OutputGuardProcessor after addPlugin");

        controller.setNodeGain (uid, 0.5f);
        r.expect (countGuards() == 1, "exactly one OutputGuardProcessor after a gain edit");

        controller.setMonoInput (true);
        controller.setMonoInput (false);
        r.expect (countGuards() == 1, "exactly one OutputGuardProcessor after repeated rewires");

        // Corrupt the persisted gain the way a hand-edited file would.
        GraphDocument corrupt = controller.snapshotDocument();
        corrupt.getNodeByUid (uid).setProperty ("gain", 1000.0f, nullptr);
        controller.loadDocument (corrupt);

        float liveGain = -1.0f;
        for (auto* node : graph.getNodes())
            if (auto* strip = dynamic_cast<NodeStripProcessor*> (node->getProcessor()))
                liveGain = jmax (liveGain, strip->getGain());
        r.expect (liveGain >= 0.0f && liveGain <= 2.0f, "live strip gain is clamped to <= 2 after loading gain 1000");
        r.expect (controller.getNodeGain (uid) <= 2.0f, "controller reports the clamped gain");
        r.expect (countGuards() == 1, "exactly one OutputGuardProcessor after loadDocument");

        controller.save (*fx.settings);
    }

    //==========================================================================
    // Test S — unreadable saved graph is backed up, never silently overwritten.
    //          A broken "graphDocument" must leave a backup file + settings key holding the
    //          original text, start from a valid default document and set a warning;
    //          a valid document must produce neither a backup nor a warning.
    //==========================================================================
    std::cout << "\n[S] unreadable settings are backed up" << std::endl;
    {
        const auto backupsFor = [] (const File& settingsFile)
        {
            return settingsFile.getParentDirectory().findChildFiles (File::findFiles, false,
                settingsFile.getFileNameWithoutExtension() + ".graphDocument-unreadable-*.xml");
        };

        const String broken = "<GRAPH broken";
        {
            GraphFixture fx (formatManager, "unreadablesettings");
            for (auto& f : backupsFor (fx.file)) f.deleteFile();

            // Valid document first: no backup, no warning.
            fx.controller->save (*fx.settings);
            GraphController good (fx.graph, formatManager);
            good.loadFrom (*fx.settings);
            r.expect (good.getLoadWarning().isEmpty(), "valid document: no warning");
            r.expect (backupsFor (fx.file).isEmpty(), "valid document: no backup file");

            // Now corrupt it.
            fx.settings->setValue ("graphDocument", broken);
            GraphController bad (fx.graph, formatManager);
            bad.loadFrom (*fx.settings);

            const auto backups = backupsFor (fx.file);
            r.expect (backups.size() == 1, "broken document: one backup file written");
            r.expect (backups.size() == 1 && backups.getReference (0).loadFileAsString() == broken,
                      "broken document: the backup holds the original text");
            r.expect (fx.settings->getValue ("graphDocument.unreadable") == broken,
                      "broken document: settings key holds the original text");
            r.expect (bad.getLoadWarning().isNotEmpty(), "broken document: warning is set");
            r.expect (bad.snapshotDocument().isValid(), "broken document: started from a valid default document");

            for (auto& f : backups) f.deleteFile();
        }
    }

    //==========================================================================
    // Test T — replace a plugin in place (v1.2 A). A VST2 stand-in carrying a synthetic FxBk
    //          bank is wired IN -> stand-in -> OUT with non-default gain / pan / mix / bypass,
    //          then replaced by a real VST3: same uid and wiring, strip values kept, real
    //          plugin live, the original state kept once, parameters imported by position,
    //          exactly one publish. A replacement that can't load changes and publishes nothing.
    //==========================================================================
    std::cout << "\n[T] replace plugin in place" << std::endl;
    {
        GraphFixture fx (formatManager, "replaceplugin");
        GraphController& controller = *fx.controller;

        // Synthetic VST2 bank: 1 program, 4 params.
        const std::vector<float> wanted { 0.25f, 0.5f, 0.75f, 1.0f };
        MemoryBlock bank;
        {
            const auto be = [&bank] (uint32_t v) { for (int s = 24; s >= 0; s -= 8) { const uint8_t b = (uint8_t) (v >> s); bank.append (&b, 1); } };
            const auto tag = [&bank] (const char* t) { bank.append (t, 4); };
            tag ("CcnK"); be (0); tag ("FxBk"); be (2); be (0x4c484f54); be (1); be (1); be (0);
            while (bank.getSize() < 156) { const uint8_t z = 0; bank.append (&z, 1); }
            tag ("CcnK"); be (0); tag ("FxCk"); be (1); be (0x4c484f54); be (1); be ((uint32_t) wanted.size());
            char name[28] = {}; std::memcpy (name, "Synthetic", 9);
            bank.append (name, 28);
            for (float f : wanted) { uint32_t u; std::memcpy (&u, &f, 4); be (u); }
        }

        PluginDescription oldVst2 = pA;
        oldVst2.name             = "OldVst2Plugin";
        oldVst2.pluginFormatName = "VST";
        oldVst2.fileOrIdentifier = File::getSpecialLocation (File::tempDirectory)
                                       .getChildFile ("lighthost_no_such_plugin_vst2.dll").getFullPathName();

        const String uid = controller.appendToChain (oldVst2);
        r.expect (uid.isNotEmpty() && controller.isMissing (uid), "replace-test stand-in created");
        controller.document.setPluginState (uid, bank.toBase64Encoding());
        controller.setNodeGain (uid, 0.7f);
        controller.setNodePan (uid, 0.3f);
        controller.setNodeMix (uid, 0.6f);
        controller.setBypassed (uid, true);

        const auto sameEdges = [] (const std::vector<GraphController::Connection>& a, const std::vector<GraphController::Connection>& b)
        {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i].srcUid != b[i].srcUid || a[i].srcCh != b[i].srcCh || a[i].dstUid != b[i].dstUid || a[i].dstCh != b[i].dstCh)
                    return false;
            return true;
        };
        const auto edgesBefore = controller.getConnections();

        // 1. A replacement that can't load: !ok, nothing changes, nothing publishes.
        PluginDescription bogus = pA;
        bogus.name             = "DeliberatelyMissing";
        bogus.fileOrIdentifier = File::getSpecialLocation (File::tempDirectory)
                                     .getChildFile ("lighthost_no_such_plugin_zzz.vst3").getFullPathName();
        const String docBefore = controller.document.toXml();
        int before = controller.getPublishCount();
        r.expect (! controller.replacePlugin (uid, bogus).ok, "replace with an unloadable plugin fails");
        r.expect (controller.getPublishCount() == before, "failed replace publishes nothing");
        r.expect (controller.document.toXml() == docBefore, "failed replace leaves the document untouched");
        r.expect (controller.isMissing (uid), "failed replace keeps the stand-in");

        // 2. Replace by a real VST3.
        before = controller.getPublishCount();
        const auto res = controller.replacePlugin (uid, pA);
        r.expect (res.ok, "replace by a real plugin succeeds");
        r.expect (controller.getPublishCount() - before == 1, "a successful replace publishes exactly once");
        r.expect (! controller.isMissing (uid), "replaced node is no longer a stand-in");
        auto* node = controller.getNodeForUid (uid);
        r.expect (node != nullptr && dynamic_cast<PassThroughProcessor*> (node->getProcessor()) == nullptr,
                  "replaced node runs the real plugin");
        r.expect (node != nullptr && node->isBypassed(), "replaced node keeps its bypass flag");
        r.expect (sameEdges (edgesBefore, controller.getConnections()), "replace keeps the wiring");
        r.expect (controller.getNodeGain (uid) == 0.7f && controller.getNodePan (uid) == 0.3f
                  && std::abs (controller.getNodeMix (uid) - 0.6f) < 1.0e-6f, "replace keeps gain / pan / mix");
        r.expect (controller.getNodeMeter (uid) != nullptr, "replaced node has its strip meter again");
        r.expect (controller.document.getPreviousPluginState (uid) == bank.toBase64Encoding(),
                  "the original VST2 state is kept as prevState");
        const auto newDesc = GraphDocument::getPluginDescription (controller.document.getNodeByUid (uid));
        r.expect (newDesc.has_value() && newDesc->pluginFormatName == pA.pluginFormatName, "document holds the new description");

        // 3. Import: parameters by position (pA is VST3 and has a few parameters).
        if (pA.pluginFormatName == "VST3" && controller.getNodeParameters (uid, false).size() >= 2)
        {
            r.expect (res.import == GraphController::ReplaceResult::Import::paramsByIndex, "a params bank imports by position");
            r.expect (res.changedParams >= 1 && res.changedParams <= (int) wanted.size(), "import reports a plausible changed count");
            r.expect (res.message.isNotEmpty(), "replace reports a one-line result");
        }

        // 4. Replacing again keeps the FIRST original, and works on a live plugin (no import).
        before = controller.getPublishCount();
        const auto again = controller.replacePlugin (uid, pA);
        r.expect (again.ok && again.import == GraphController::ReplaceResult::Import::none, "live-to-live replace works without import");
        r.expect (controller.getPublishCount() - before == 1, "second replace publishes exactly once");
        r.expect (controller.document.getPreviousPluginState (uid) == bank.toBase64Encoding(),
                  "a second replace keeps the first prevState");
        r.expect (sameEdges (edgesBefore, controller.getConnections()), "second replace keeps the wiring");

        // 5. Empty state + fallbackState (a node re-added from a stale VST2 entry; the original sits in the
        //    legacy settings key): the import still happens and the fallback is kept as prevState.
        {
            const String blankUid = controller.appendToChain (oldVst2);
            r.expect (blankUid.isNotEmpty() && controller.isMissing (blankUid)
                      && controller.document.getPluginState (blankUid).isEmpty(), "stateless stand-in created");
            before = controller.getPublishCount();
            const auto viaFallback = controller.replacePlugin (blankUid, pA, &bank);
            r.expect (viaFallback.ok, "replace of a stateless stand-in with a fallback succeeds");
            r.expect (controller.getPublishCount() - before == 1, "fallback replace publishes exactly once");
            if (pA.pluginFormatName == "VST3" && controller.getNodeParameters (blankUid, false).size() >= 2)
                r.expect (viaFallback.import == GraphController::ReplaceResult::Import::paramsByIndex
                          && viaFallback.changedParams >= 1, "the fallback state is imported by position");
            r.expect (controller.document.getPreviousPluginState (blankUid) == bank.toBase64Encoding(),
                      "prevState holds the fallback state");
        }

        controller.save (*fx.settings);
    }

    //==========================================================================
    // Test V (real plugin) — the two-OUT render on the real graph: a real effect (bypassed, so the result is
    //          deterministic) fed by the graph input drives the main OUT and an extra OUT on pair 2/3; both
    //          pairs carry the same signal and the input keeps flowing to the main pair when the layout
    //          changes underneath.
    //==========================================================================
    std::cout << "\n[V] two OUTs on the real graph" << std::endl;
    {
        GraphFixture fx (formatManager, "realoutputs", 44100.0, 512, 4);
        GraphController& controller = *fx.controller;
        lighthost::outputs::Layout layout;
        layout.known = true; layout.deviceName = "TestDevice"; layout.activeMask.parseString ("1111", 2);
        layout.names = StringArray { "Out 1", "Out 2", "Loop-back 1", "Loop-back 2" };
        controller.setOutputLayout (layout);

        const String uid = controller.appendToChain (pA);
        r.expect (uid.isNotEmpty(), "real effect added");
        controller.setBypassed (uid, true);
        const String out2 = controller.addOutput (2);
        controller.connectPair (uid, out2);

        AudioBuffer<float> block (4, 512);
        Random rng (0x0b0b);
        float peak[4] = {};
        for (int b = 0; b < 16; ++b)
        {
            block.clear();
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    block.setSample (ch, i, 0.25f * (rng.nextFloat() * 2.0f - 1.0f));
            MidiBuffer midi;
            fx.graph.processBlock (block, midi);
            for (int ch = 0; ch < 4; ++ch)
                peak[ch] = jmax (peak[ch], block.getMagnitude (ch, 0, 512));
        }
        r.expect (peak[0] > 0.05f && peak[1] > 0.05f, "the main pair carries the input through the real effect");
        r.expect (peak[2] > 0.05f && peak[3] > 0.05f, "the extra pair carries the same signal");
        r.expect (std::abs (peak[0] - peak[2]) < 0.02f, "both pairs carry the same level");

        controller.setOutputLayout ([&layout]() { auto l = layout; l.activeMask.parseString ("0011", 2); return l; }());
        block.clear();
        for (int i = 0; i < 512; ++i) { block.setSample (0, i, 0.25f); block.setSample (1, i, 0.25f); }
        MidiBuffer midi;
        fx.graph.processBlock (block, midi);
        r.expect (block.getMagnitude (2, 0, 512) < 1.0e-6f && block.getMagnitude (0, 0, 512) > 0.05f,
                  "with pair 2/3 inactive the extra OUT is silent and the main pair keeps playing");
        controller.save (*fx.settings);
    }

    //==========================================================================
    // Test U (real VST3 instrument) — if the machine has one: it loads as a source, is wired to the main OUT,
    //          receives MIDI and survives a note. Silence is reported, not failed (a sampler without its
    //          library is silent by design).
    //==========================================================================
    std::cout << "\n[U] real VST3 instrument" << std::endl;
    {
        const auto instruments = discoverPlugins (formatManager, explicitPluginPaths, 1, /*instruments*/ true);
        if (instruments.isEmpty())
        {
            std::cout << "  skipped (no instrument)" << std::endl;
        }
        else
        {
            std::cout << "  using instrument: " << instruments.getReference (0).name << std::endl;
            GraphFixture fx (formatManager, "realinstrument");
            GraphController& controller = *fx.controller;
            const String uid = controller.addPlugin (instruments.getReference (0));
            r.expect (uid.isNotEmpty() && ! controller.isMissing (uid), "the real instrument loads");
            r.expect (controller.document.isSourceNode (uid), "it reads as a source");
            r.expect (controller.document.hasConnection (uid, 0, controller.document.getIoNodeUid (false), 0), "it is wired to the main OUT");
            r.expect (controller.getNodeReceivesMidi (uid) && controller.nodeAcceptsMidi (uid), "it receives MIDI");

            float peak = 0.0f;
            AudioBuffer<float> block (2, 512);
            for (int b = 0; b < 64; ++b)
            {
                block.clear();
                MidiBuffer midi;
                if (b == 0) midi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 0);
                fx.graph.processBlock (block, midi);
                peak = jmax (peak, peakOf (block, 0), peakOf (block, 1));
            }
            std::cout << "  note peak " << peak << (peak > 1.0e-6f ? "" : "  (silent: instrument may need its sound library)") << std::endl;
            r.expect (std::isfinite (peak), "the note renders finite audio");
        }
    }

    //==========================================================================
    // Test X - plugin scan in a child process (v1.2). One worker per file: a worker that dies or hangs
    //          before its first reply blacklists exactly that file; one that hangs during the test load
    //          keeps its descriptions and is not blacklisted. The faults come from LIGHTHOST_SCAN_WORKER_TEST.
    //          Nothing here scans the VST3 folder, and nothing runs a plugin that may crash in this process.
    //==========================================================================
    std::cout << "\n[X] out-of-process plugin scan" << std::endl;
    {
        const File exe = findScanWorkerExe();
        AudioPluginFormat* vst3 = nullptr;
        for (auto* f : formatManager.getFormats())
            if (f->getName() == "VST3")
                vst3 = f;

        r.expect (exe.existsAsFile(), "the scan worker executable exists: " + exe.getFullPathName());

        if (exe.existsAsFile() && vst3 != nullptr)
        {
            const auto known = discoverPlugins (formatManager, explicitPluginPaths, 1);
            const bool haveReal = ! known.isEmpty();
            const String path = haveReal ? known.getReference (0).fileOrIdentifier
                                         : File::getSpecialLocation (File::tempDirectory).getChildFile ("lighthost_scan_fault.vst3").getFullPathName();
            std::cout << "  file: " << path << (haveReal ? "" : "  (no plugin found: fault paths only)") << std::endl;

            const auto millis = [] { return Time::getMillisecondCounter(); };
            const lighthost::scan::Timeouts quick { 1500, 1500 };

            setEnv (lighthost::scan::kTestEnvVar, "");

            if (haveReal)
            {
                lighthost::scan::OutOfProcessScanner scanner (exe);
                OwnedArray<PluginDescription> viaWorker, inProcess;
                const auto t0 = millis();
                const bool ok = scanner.findPluginTypesFor (*vst3, viaWorker, path);
                const auto ms = millis() - t0;
                vst3->findAllTypesForFile (inProcess, path);   // one file that already loads in this process

                std::cout << "  one scan through a worker: " << ms << " ms" << std::endl;
                r.expect (ok && ! viaWorker.isEmpty(), "a known-good file scans through a worker");
                bool same = viaWorker.size() == inProcess.size();
                for (int i = 0; same && i < viaWorker.size(); ++i)
                    same = viaWorker[i]->name == inProcess[i]->name && viaWorker[i]->uniqueId == inProcess[i]->uniqueId
                        && viaWorker[i]->isInstrument == inProcess[i]->isInstrument
                        && viaWorker[i]->numInputChannels == inProcess[i]->numInputChannels
                        && viaWorker[i]->numOutputChannels == inProcess[i]->numOutputChannels;
                r.expect (same, "the descriptions equal the in-process ones");
            }

            {
                setEnv (lighthost::scan::kTestEnvVar, "abort");
                KnownPluginList list;
                list.setCustomScanner (std::make_unique<lighthost::scan::OutOfProcessScanner> (exe, quick));
                OwnedArray<PluginDescription> found;
                const auto t0 = millis();
                list.scanAndAddFile (path, false, found, *vst3);
                r.expect (found.isEmpty() && list.getBlacklistedFiles().contains (path),
                          "a worker that dies before replying: the file is blacklisted, this process lives");
                const auto ms = millis() - t0;
                r.expect (ms < 1400, "a dead worker is noticed at once, not after the timeout (" + String ((int) ms) + " ms)");
            }

            {
                setEnv (lighthost::scan::kTestEnvVar, "hang");
                KnownPluginList list;
                list.setCustomScanner (std::make_unique<lighthost::scan::OutOfProcessScanner> (exe, quick));
                OwnedArray<PluginDescription> found;
                const auto t0 = millis();
                list.scanAndAddFile (path, false, found, *vst3);
                const auto ms = millis() - t0;
                r.expect (found.isEmpty() && list.getBlacklistedFiles().contains (path),
                          "a worker that never answers: the file is blacklisted after the timeout");
                r.expect (ms >= 1400 && ms < 15000, "the timeout path took about the timeout (" + String ((int) ms) + " ms)");
            }

            {
                setEnv (lighthost::scan::kTestEnvVar, "hang-instantiate");
                KnownPluginList list;
                list.setCustomScanner (std::make_unique<lighthost::scan::OutOfProcessScanner> (exe, lighthost::scan::Timeouts { 30000, 1500 }));
                OwnedArray<PluginDescription> found;
                list.scanAndAddFile (path, false, found, *vst3);
                r.expect (! list.getBlacklistedFiles().contains (path), "a test load that never finishes does not blacklist the file");
                if (haveReal)
                    r.expect (! found.isEmpty() && list.getNumTypes() == found.size(), "... and the descriptions are kept");
            }

            setEnv (lighthost::scan::kTestEnvVar, "");

            // HALion Sonic dies on instantiation, on this machine only: run it when asked (LIGHTHOST_TEST_HALION=1).
            if (SystemStats::getEnvironmentVariable ("LIGHTHOST_TEST_HALION", {}) == "1")
            {
                const String halion = "C:\\Program Files\\Common Files\\VST3\\Steinberg\\HALion Sonic.vst3";
                KnownPluginList list;
                list.setCustomScanner (std::make_unique<lighthost::scan::OutOfProcessScanner> (exe));
                OwnedArray<PluginDescription> found;
                list.scanAndAddFile (halion, false, found, *vst3);
                std::cout << "  HALion Sonic: " << (list.getBlacklistedFiles().contains (halion) ? "blacklisted" : "kept") << std::endl;
                r.expect (true, "HALion Sonic through a worker did not take this process down");
            }
        }
    }

    //==========================================================================
    std::cout << "\n" << (r.failures == 0 ? "PASS " : "FAIL ")
              << (r.checks - r.failures) << "/" << r.checks << " checks" << std::endl;
    std::cout.flush();
    return r.failures == 0 ? 0 : 1;
}

} // namespace lighthost
