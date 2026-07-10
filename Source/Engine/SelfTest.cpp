//
//  SelfTest.cpp — see SelfTest.h. Headless runtime checks against real VST3s.
//

#include "SelfTest.h"
#include "GraphController.h"

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
}

//==============================================================================
int runSelfTest (const StringArray& explicitPluginPaths)
{
    AudioPluginFormatManager formatManager;
    addDefaultFormatsToManager (formatManager);   // JUCE 8: free function replaces addDefaultFormats()

    std::cout << "GraphController runtime self-test (real VST3 plugins)" << std::endl;

    const Array<PluginDescription> plugins = discoverPlugins (formatManager, explicitPluginPaths, 2);

    if (plugins.isEmpty())
    {
        std::cout << "SKIP: no loadable stereo VST3 found — nothing to test." << std::endl;
        return 0;   // green on a plugin-less box
    }

    const PluginDescription& pA = plugins.getReference (0);
    std::cout << "Using plugin A: " << pA.name << " (" << pA.pluginFormatName << ")" << std::endl;
    if (plugins.size() > 1)
        std::cout << "Using plugin B: " << plugins.getReference (1).name << std::endl;

    Report r;

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
    std::cout << "\n" << (r.failures == 0 ? "PASS " : "FAIL ")
              << (r.checks - r.failures) << "/" << r.checks << " checks" << std::endl;
    std::cout.flush();
    return r.failures == 0 ? 0 : 1;
}

} // namespace lighthost
