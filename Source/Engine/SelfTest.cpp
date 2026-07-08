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
    std::cout << "\n" << (r.failures == 0 ? "PASS " : "FAIL ")
              << (r.checks - r.failures) << "/" << r.checks << " checks" << std::endl;
    std::cout.flush();
    return r.failures == 0 ? 0 : 1;
}

} // namespace lighthost
