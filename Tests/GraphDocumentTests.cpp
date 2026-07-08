//
//  GraphDocumentTests.cpp — headless unit tests for the milestone-2 graph
//  document model. No audio device, no real plugins, no display: this exercises
//  GraphDocument's persistence and migration logic only, so it runs anywhere.
//
//  Build:  cmake -B build -DLIGHTHOST_BUILD_TESTS=ON && cmake --build build --target LightHostTests
//  Run:    ./build/LightHostTests_artefacts/LightHostTests   (exit code 0 = all passed)
//

#include "../Source/Engine/GraphDocument.h"
#include <iostream>

int runMeterTests (int& checks);   // Tests/MeterTests.cpp

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg)                                             \
    do {                                                            \
        ++checks;                                                   \
        if (! (cond)) { std::cerr << "  FAIL: " << (msg) << '\n'; ++failures; } \
    } while (0)

// A synthetic VST3-ish description; two calls with the same name model the
// "same plugin twice" case that collided under the legacy PropertiesFile keys.
static PluginDescription makeDesc (const String& name)
{
    PluginDescription d;
    d.name              = name;
    d.pluginFormatName  = "VST3";
    d.category          = "Fx";
    d.manufacturerName  = "TestCo";
    d.version           = "1.0";
    d.fileOrIdentifier  = "/fake/" + name + ".vst3";
    d.uniqueId          = name.hashCode();
    d.deprecatedUid     = name.hashCode();
    d.isInstrument      = false;
    d.numInputChannels  = 2;
    d.numOutputChannels = 2;
    return d;
}

int main()
{
    std::cout << "GraphDocument unit tests\n";

    // --- 1. Empty document is valid and has both IO nodes -------------------
    GraphDocument doc;
    CHECK (doc.isValid(), "empty document is valid");
    CHECK (doc.getIoNodeUid (true).isNotEmpty(),  "audioIn uid present");
    CHECK (doc.getIoNodeUid (false).isNotEmpty(), "audioOut uid present");
    CHECK (doc.getNodes().getNumChildren() == 2,  "empty document has exactly 2 IO nodes");

    // --- 2. Duplicate plugin instances get distinct UUIDs ------------------
    const auto desc = makeDesc ("Reverb");
    const String u1 = doc.addPluginNode (desc, 0.3f, 0.5f);
    const String u2 = doc.addPluginNode (desc, 0.6f, 0.5f);
    CHECK (u1.isNotEmpty() && u2.isNotEmpty(), "plugin node uids non-empty");
    CHECK (u1 != u2, "two instances of the same plugin get distinct uids");
    CHECK (doc.getNodes().getNumChildren() == 4, "2 IO + 2 plugin nodes");

    // --- 3. Bypass and state are per-node and independent ------------------
    doc.setBypassed (u1, true);
    doc.setPluginState (u2, "QUJDRA==");   // arbitrary base64
    CHECK (doc.isBypassed (u1),  "u1 bypassed");
    CHECK (! doc.isBypassed (u2), "u2 not bypassed (independent)");
    CHECK (doc.getPluginState (u2) == "QUJDRA==", "u2 state stored");
    CHECK (doc.getPluginState (u1).isEmpty(),      "u1 has no state");

    // --- 4. Serial connections: in -> u1 -> u2 -> out ----------------------
    doc.addConnection (doc.getIoNodeUid (true), 0, u1, 0);
    doc.addConnection (doc.getIoNodeUid (true), 1, u1, 1);
    doc.addConnection (u1, 0, u2, 0);
    doc.addConnection (u1, 1, u2, 1);
    doc.addConnection (u2, 0, doc.getIoNodeUid (false), 0);
    doc.addConnection (u2, 1, doc.getIoNodeUid (false), 1);
    CHECK (doc.getConnections().getNumChildren() == 6, "6 stereo connections");

    // --- 5. XML round-trip is loss-free -----------------------------------
    const String xml = doc.toXml();
    GraphDocument doc2 = GraphDocument::fromXml (xml);
    CHECK (doc2.isValid(),          "round-tripped document is valid");
    CHECK (doc2.toXml() == xml,     "round-trip produces identical XML");
    CHECK (doc2.isBypassed (u1),    "bypass survives round-trip");
    CHECK (doc2.getPluginState (u2) == "QUJDRA==", "state survives round-trip");
    const auto rd = GraphDocument::getPluginDescription (doc2.getNodeByUid (u1));
    CHECK (rd.has_value() && rd->name == "Reverb", "PluginDescription survives round-trip");

    // --- 6. Invalid XML yields an invalid document ------------------------
    GraphDocument bad = GraphDocument::fromXml ("<NOT_A_GRAPH/>");
    CHECK (! bad.isValid(), "garbage XML gives an invalid document");

    // --- 7. removeNode also drops that node's connections -----------------
    doc.removeNode (u1);
    CHECK (! doc.getNodeByUid (u1).isValid(), "removed node is gone");
    // Only the u2->out pair should remain (in->u1 and u1->u2 both involved u1).
    CHECK (doc.getConnections().getNumChildren() == 2, "connections involving removed node are pruned");

    // --- 8. Migration from legacy PropertiesFile settings -----------------
    {
        auto tmpDir = File::getSpecialLocation (File::tempDirectory)
                          .getChildFile ("lighthost_migrate_test");
        tmpDir.createDirectory();
        auto settingsFile = tmpDir.getChildFile ("legacy.settings");
        settingsFile.deleteFile();

        PropertiesFile::Options opts;
        opts.applicationName     = "legacy";
        opts.filenameSuffix      = "settings";
        opts.folderName          = tmpDir.getFullPathName();
        opts.osxLibrarySubFolder = "Application Support";
        PropertiesFile settings (settingsFile, opts);

        // Legacy active plugin list.
        KnownPluginList kpl;
        kpl.addType (makeDesc ("Comp"));
        kpl.addType (makeDesc ("EQ"));
        if (auto listXml = kpl.createXml())
            settings.setValue ("pluginListActive", listXml.get());

        // Legacy per-plugin keys: order (unix ts), bypass, state.
        const auto key = [] (const String& t, const PluginDescription& p)
        { return "plugin-" + t + "-" + p.name + p.version + p.pluginFormatName; };

        settings.setValue (key ("order",  makeDesc ("Comp")), 100);   // earlier
        settings.setValue (key ("order",  makeDesc ("EQ")),   200);   // later
        settings.setValue (key ("bypass", makeDesc ("EQ")),   true);
        settings.setValue (key ("state",  makeDesc ("Comp")), "U1RBVEU=");

        GraphDocument migrated = GraphDocument::migrateFromLegacySettings (settings);
        CHECK (migrated.isValid(), "migrated document is valid");
        CHECK (migrated.getNodes().getNumChildren() == 4, "migration produced 2 IO + 2 plugin nodes");

        // Order preserved: Comp (100) before EQ (200) in node order.
        int compIdx = -1, eqIdx = -1;
        auto nodes = migrated.getNodes();
        for (int i = 0; i < nodes.getNumChildren(); ++i)
        {
            const auto n = nodes.getChild (i);
            if (n.getProperty ("name").toString() == "Comp") compIdx = i;
            if (n.getProperty ("name").toString() == "EQ")   eqIdx   = i;
        }
        CHECK (compIdx >= 0 && eqIdx >= 0 && compIdx < eqIdx, "legacy timestamp order preserved");

        // Full serial chain wired: in + 2 plugins + out = 3 hops x 2 channels.
        CHECK (migrated.getConnections().getNumChildren() == 6, "migration wires a stereo serial chain");

        settingsFile.deleteFile();
        tmpDir.deleteRecursively();
    }

    failures += runMeterTests (checks);

    std::cout << (failures == 0 ? "PASS " : "FAIL ")
              << (checks - failures) << '/' << checks << " checks\n";
    return failures == 0 ? 0 : 1;
}
