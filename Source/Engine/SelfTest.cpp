//
//  SelfTest.cpp — see SelfTest.h. Headless runtime checks against real VST3s.
//

#include "SelfTest.h"
#include "GraphController.h"
#include "PresetStore.h"

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

    // Locate loadable stereo FX VST3s. Returns their PluginDescriptions.
    Array<PluginDescription> discoverPlugins (AudioPluginFormatManager& fm,
                                              const StringArray& explicitPaths,
                                              int wanted)
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
                    // The host only routes stereo FX (channels 0-1), no instruments.
                    if (! desc->isInstrument
                        && desc->numInputChannels  >= 2
                        && desc->numOutputChannels >= 2)
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

        // Fill `block` with white noise, process `count` blocks, and return the
        // highest ch0 peak the given meter saw. Processing many blocks lets any
        // plugin latency fill through before we sample the meter.
        auto driveNoise = [&] (const MeterTap* meter, int count)
        {
            float maxPeak = 0.0f;
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
                if (meter != nullptr)
                    maxPeak = jmax (maxPeak, meter->read().peak[0]);
            }
            return maxPeak;
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
        controller.removeFromChain (uid);           // rebuild -> fresh meter nodes
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
                if (mA   != nullptr) maxA   = jmax (maxA,   mA->read().peak[0]);
                if (mB   != nullptr) { lastB = mB->read().peak[0]; maxB = jmax (maxB, lastB); }
                if (mOut != nullptr) maxOut = jmax (maxOut, mOut->read().peak[0]);
            }
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
            controller.removeFromChain (uid);
        }

        if (midiUid.isNotEmpty())
        {
            r.expect (controller.getNodeReceivesMidi (midiUid),
                      "receivesMidi defaults to true on a MIDI-accepting plugin");
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
            r.expect (controller.getNodeReceivesMidi (uid), "receivesMidi defaults to true");
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

        // Distinct, non-default positions (getNodeX/Y default to 0.5f, so avoid 0.5).
        controller.setNodePosition (uid1, 0.1f, 0.2f);
        controller.setNodePosition (uid2, 0.8f, 0.9f);

        // A structural edit that rebuilds the graph: add a third node then remove it.
        const String uid3 = controller.appendToChain (pA);
        r.expect (uid3.isNotEmpty(), "third plugin instantiated");
        controller.removeFromChain (uid3);

        const auto eq = [] (float a, float b) { return std::abs (a - b) < 1.0e-6f; };
        r.expect (eq (controller.getNodeX (uid1), 0.1f) && eq (controller.getNodeY (uid1), 0.2f),
                  "node 1 position unchanged after append + remove");
        r.expect (eq (controller.getNodeX (uid2), 0.8f) && eq (controller.getNodeY (uid2), 0.9f),
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
            float maxPeak = 0.0f;
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
                if (outMeter != nullptr)
                    maxPeak = jmax (maxPeak, outMeter->read().peak[0]);
            }
            return maxPeak;
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

        controller.removeNode (p2);
        r.expect (! controller.document.getNodeByUid (p2).isValid(), "deleted node left the document");

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
        auto driveNoise = [&] (int count)
        {
            float maxPeak = 0.0f;
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
                if (const MeterTap* m = controller.getOutputMeter())   // re-fetch: rebuilt on delete
                    maxPeak = jmax (maxPeak, m->read().peak[0]);
            }
            return maxPeak;
        };
        r.expect (driveNoise (16) > 0.0f, "audio still reaches the output through the healed graph");

        // Linear heal: deleting the remaining plugin bridges in -> out directly.
        controller.removeNode (p1);
        r.expect (controller.document.hasConnection (in, 0, out, 0)
               && controller.document.hasConnection (in, 1, out, 1),
                  "deleting the last plugin heals in->out on both channels");
        r.expect (driveNoise (16) > 0.0f, "bare in->out still carries audio after the second delete");

        controller.save (*settings);
        f.deleteFile();
    }

    //==========================================================================
    std::cout << "\n" << (r.failures == 0 ? "PASS " : "FAIL ")
              << (r.checks - r.failures) << "/" << r.checks << " checks" << std::endl;
    std::cout.flush();
    return r.failures == 0 ? 0 : 1;
}

} // namespace lighthost
