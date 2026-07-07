//
//  GraphRuntimeTests.cpp — headless runtime tests for GraphController that use
//  REAL installed VST3 plugins (no tray menu, no display interaction).
//
//  These cover the three behaviours the pure document-model tests can't, because
//  they need actual plugin instantiation through the AudioProcessorGraph:
//    A. two instances of the SAME plugin coexist as distinct live nodes
//       (the upstream name+version+format key made this impossible);
//    B. a bypass flag survives a save() -> fresh-controller loadFrom() cycle;
//    C. a pre-2026 legacy settings file (pluginListActive + plugin-order/
//       bypass/state-* keys, no graphDocument) migrates on first load, then
//       persists as a graphDocument on save.
//
//  Plugins are discovered from the standard VST3 folder at runtime, so the test
//  is not pinned to one machine's plugin set. It needs at least one loadable
//  stereo VST3; if none is found it SKIPs (exit 0) rather than failing, so CI on
//  a plugin-less box stays green.
//
//  Build:  cmake -B build -DLIGHTHOST_BUILD_RUNTIME_TESTS=ON
//          cmake --build build --config Release --target LightHostRuntimeTests
//  Run:    "build/LightHostRuntimeTests_artefacts/Release/LightHostRuntimeTests.exe"
//          (optionally pass one or more .vst3 paths as arguments)
//

#include <JuceHeader.h>
#include "../Source/Engine/GraphController.h"

using namespace juce;

//==============================================================================
namespace
{
    int  checks   = 0;
    int  failures = 0;

    void expect (bool condition, const String& message)
    {
        ++checks;
        if (condition)
        {
            std::cout << "  ok   " << message << std::endl;
        }
        else
        {
            ++failures;
            std::cout << "  FAIL " << message << std::endl;
        }
    }

    // Locate a couple of loadable stereo VST3s. Returns their PluginDescriptions.
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
            File vst3Dir (File::getSpecialLocation (File::userHomeDirectory)
                              .getChildFile (".vst3"));
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

    // Minimal legacy (pre-2026) settings file: pluginListActive + the flat
    // plugin-order/bypass/state-* keys, exactly as upstream LightHost wrote them.
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
            settings.setValue (key ("order",  p), String (1000 + i));       // legacy: unix-ish order value
            settings.setValue (key ("bypass", p), bypass[(size_t) i]);
        }

        settings.saveIfNeeded();
    }

    File tempSettingsFile (const String& name)
    {
        return File::getSpecialLocation (File::tempDirectory)
                   .getChildFile ("LightHostRuntimeTest_" + name + ".settings");
    }

    std::unique_ptr<PropertiesFile> makeSettings (const File& f)
    {
        PropertiesFile::Options o;
        o.commonToAllUsers = false;
        return std::make_unique<PropertiesFile> (f, o);
    }
}

//==============================================================================
int main (int argc, char* argv[])
{
    ScopedJuceInitialiser_GUI juceInit;   // message manager for plugin instantiation

    StringArray explicitPaths;
    for (int i = 1; i < argc; ++i)
        explicitPaths.add (argv[i]);

    AudioPluginFormatManager formatManager;
    addDefaultFormatsToManager (formatManager);   // JUCE 8: free function replaces addDefaultFormats()

    std::cout << "GraphController runtime tests (real VST3 plugins)" << std::endl;

    const Array<PluginDescription> plugins = discoverPlugins (formatManager, explicitPaths, 2);

    if (plugins.isEmpty())
    {
        std::cout << "SKIP: no loadable stereo VST3 found — nothing to test." << std::endl;
        return 0;   // green on a plugin-less box
    }

    const PluginDescription& pA = plugins.getReference (0);
    std::cout << "Using plugin A: " << pA.name << " (" << pA.pluginFormatName << ")" << std::endl;
    if (plugins.size() > 1)
        std::cout << "Using plugin B: " << plugins.getReference (1).name << std::endl;

    //==========================================================================
    // Test A — two instances of the SAME plugin coexist as distinct live nodes.
    //==========================================================================
    std::cout << "\n[A] duplicate-plugin coexistence" << std::endl;
    String uidA1, uidA2;
    {
        AudioProcessorGraph graph;
        graph.setPlayConfigDetails (2, 2, 44100.0, 512);
        graph.prepareToPlay (44100.0, 512);

        GraphController controller (graph, formatManager);
        const File f = tempSettingsFile ("dup");
        f.deleteFile();
        auto settings = makeSettings (f);
        controller.loadFrom (*settings);   // empty/default document

        uidA1 = controller.appendToChain (pA);
        uidA2 = controller.appendToChain (pA);

        expect (uidA1.isNotEmpty(),          "first instance instantiated (non-empty uid)");
        expect (uidA2.isNotEmpty(),          "second instance instantiated (non-empty uid)");
        expect (uidA1 != uidA2,              "the two instances have distinct uids");

        const auto chain = controller.getChain();
        expect (chain.size() == 2,           "chain holds exactly two plugins");
        if (chain.size() == 2)
        {
            expect (! chain[0].missing && ! chain[1].missing,
                    "both instances are live (not missing)");
            expect (chain[0].name == pA.name && chain[1].name == pA.name,
                    "both instances name the same plugin");
        }

        auto* n1 = controller.getNodeForUid (uidA1);
        auto* n2 = controller.getNodeForUid (uidA2);
        expect (n1 != nullptr && n2 != nullptr && n1 != n2,
                "both map to distinct live graph nodes");

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

        // Session 1: build a two-plugin chain, bypass the first, save.
        {
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
            expect (node != nullptr && node->isBypassed(),
                    "live node reports bypassed immediately (pass-through, no rebuild)");

            controller.save (*settings);
            settings->saveIfNeeded();
        }

        // Confirm the persisted file actually carries a graphDocument.
        {
            auto settings = makeSettings (f);
            expect (settings->getValue ("graphDocument").isNotEmpty(),
                    "graphDocument written to settings on save");
        }

        // Session 2: brand-new graph + controller, load the same file.
        {
            AudioProcessorGraph graph;
            graph.setPlayConfigDetails (2, 2, 44100.0, 512);
            graph.prepareToPlay (44100.0, 512);

            GraphController controller (graph, formatManager);
            auto settings = makeSettings (f);
            controller.loadFrom (*settings);

            const auto chain = controller.getChain();
            expect (chain.size() == 2,   "reloaded chain still has two plugins");

            if (chain.size() == 2)
            {
                expect (chain[0].uid == firstUid && chain[1].uid == secondUid,
                        "node uids are stable across restart");
                expect (chain[0].bypassed,       "first plugin is still bypassed after restart");
                expect (! chain[1].bypassed,     "second plugin is still enabled after restart");

                auto* node = controller.getNodeForUid (firstUid);
                expect (node != nullptr && node->isBypassed(),
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

        // Seed a legacy file: one plugin, bypassed, NO graphDocument.
        {
            auto settings = makeSettings (f);
            writeLegacySettings (*settings, { pA }, { true });
            expect (settings->getValue ("graphDocument").isEmpty(),
                    "seed file has no graphDocument (genuine pre-2026 layout)");
            expect (settings->getValue ("pluginListActive").isNotEmpty(),
                    "seed file has legacy pluginListActive");
        }

        // First load with the new build -> migration path.
        {
            AudioProcessorGraph graph;
            graph.setPlayConfigDetails (2, 2, 44100.0, 512);
            graph.prepareToPlay (44100.0, 512);

            GraphController controller (graph, formatManager);
            auto settings = makeSettings (f);
            controller.loadFrom (*settings);

            const auto chain = controller.getChain();
            expect (chain.size() == 1,               "migrated chain has the one legacy plugin");
            if (chain.size() == 1)
            {
                expect (chain[0].name == pA.name,    "migrated plugin name matches");
                expect (chain[0].bypassed,           "migrated bypass flag preserved");
                expect (! chain[0].missing,          "migrated plugin instantiated live");
            }

            controller.save (*settings);
            settings->saveIfNeeded();
        }

        // The legacy keys must remain (downgrade-safe) AND a graphDocument now exists.
        {
            auto settings = makeSettings (f);
            expect (settings->getValue ("graphDocument").isNotEmpty(),
                    "graphDocument persisted after migration");
            expect (settings->getValue ("pluginListActive").isNotEmpty(),
                    "legacy keys left intact (downgrade-safe)");
        }

        f.deleteFile();
    }

    //==========================================================================
    std::cout << "\n" << (failures == 0 ? "PASS " : "FAIL ")
              << (checks - failures) << "/" << checks << " checks" << std::endl;
    return failures == 0 ? 0 : 1;
}
