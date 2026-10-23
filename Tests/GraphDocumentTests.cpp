//
//  GraphDocumentTests.cpp — headless unit tests for the milestone-2 graph
//  document model. No audio device, no real plugins, no display: this exercises
//  GraphDocument's persistence and migration logic only, so it runs anywhere.
//
//  Build:  cmake -B build -DLIGHTHOST_BUILD_TESTS=ON && cmake --build build --target LightHostTests
//  Run:    ./build/LightHostTests_artefacts/LightHostTests   (exit code 0 = all passed)
//

#include "../Source/Engine/GraphDocument.h"
#include "../Source/Engine/NodeGeometry.h"
#include <iostream>

int runMeterTests (int& checks);   // Tests/MeterTests.cpp
int runStripTests (int& checks);   // Tests/StripTests.cpp
int runParamTests (int& checks);   // Tests/ParamTests.cpp
int runRoutingTests (int& checks); // Tests/RoutingTests.cpp
int runMidiTests (int& checks);    // Tests/MidiTests.cpp
int runPresetTests (int& checks);  // Tests/PresetTests.cpp
int runMixTests (int& checks);     // Tests/MixTests.cpp
int runSettingsTests (int& checks);// Tests/SettingsTests.cpp

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

        // Positions are px now: plugins step right by one slot, OUT is rightmost.
        float lastX = -1.0f;
        bool increasing = true;
        float maxPluginX = 0.0f, firstPluginX = -1.0f;
        for (int i = 0; i < nodes.getNumChildren(); ++i)
        {
            const auto n = nodes.getChild (i);
            if (n.getProperty ("type").toString() != "plugin")
                continue;
            const float x = migrated.getNodeX (n.getProperty ("uid").toString());
            if (firstPluginX < 0.0f) firstPluginX = x;
            increasing = increasing && x > lastX;
            lastX = x;
            maxPluginX = x;
        }
        CHECK (increasing, "legacy migration: plugin x increases in chain order");
        CHECK (migrated.getNodeX (migrated.getIoNodeUid (false)) > maxPluginX, "legacy migration: OUT is the rightmost node");
        CHECK (migrated.getNodeX (migrated.getIoNodeUid (true)) < firstPluginX, "legacy migration: IN is left of the plugins");

        settingsFile.deleteFile();
        tmpDir.deleteRecursively();
    }

    // --- GRAPH v1 (normalised positions) -> v2 (px) migration ----------------
    {
        // Nodes: "IN", "OUT" and plugins "P1".."Pn"; uid = the label. x/y normalised.
        struct V1Node { const char* uid; const char* type; float x, y; };
        const auto makeV1 = [] (const std::vector<V1Node>& nodes, const char* versionAttr = "version=\"1\"")
        {
            String xml = "<GRAPH " + String (versionAttr) + "><NODES>";
            for (const auto& n : nodes)
                xml << "<NODE uid=\"" << n.uid << "\" type=\"" << n.type << "\" name=\"" << n.uid
                    << "\" x=\"" << n.x << "\" y=\"" << n.y << "\" bypassed=\"0\"/>";
            xml << "</NODES><CONNECTIONS/></GRAPH>";
            return xml;
        };
        const auto nearPx = [] (float a, float b) { return std::abs (a - b) < 0.6f; };
        const auto noOverlap = [] (const GraphDocument& d)   // plugin cards only, like the migration
        {
            std::vector<juce::Rectangle<float>> rects;
            auto nodes = d.getNodes();
            for (int i = 0; i < nodes.getNumChildren(); ++i)
            {
                const auto n = nodes.getChild (i);
                const String uid = n.getProperty ("uid").toString();
                if (n.getProperty ("type").toString() == "plugin")
                    rects.push_back (lighthost::geometry::nodeRect (false, { d.getNodeX (uid), d.getNodeY (uid) }));
            }
            for (size_t a = 0; a < rects.size(); ++a)
                for (size_t b = a + 1; b < rects.size(); ++b)
                    if (rects[a].intersects (rects[b]))
                        return false;
            return true;
        };

        const auto d1 = GraphDocument::fromXml (makeV1 ({ { "IN", "audioIn", 0.05f, 0.5f }, { "P1", "plugin", 0.35f, 0.5f },
                                                          { "P2", "plugin", 0.65f, 0.5f }, { "OUT", "audioOut", 0.95f, 0.5f } }));
        CHECK (d1.isValid() && d1.migratedOnLoad, "v1 document: valid and flagged as migrated");
        CHECK ((int) d1.state.getProperty ("version") == 2, "v1 document: version becomes 2");
        CHECK (nearPx (d1.getNodeX ("IN"), 186.0f) && nearPx (d1.getNodeY ("IN"), 350.0f), "v1 document: IN at (186, 350)");
        CHECK (nearPx (d1.getNodeX ("P1"), 462.0f) && nearPx (d1.getNodeY ("P1"), 350.0f), "v1 document: P1 at (462, 350)");
        CHECK (nearPx (d1.getNodeX ("P2"), 738.0f) && nearPx (d1.getNodeY ("P2"), 350.0f), "v1 document: P2 at (738, 350)");
        CHECK (nearPx (d1.getNodeX ("OUT"), 1014.0f) && nearPx (d1.getNodeY ("OUT"), 350.0f), "v1 document: OUT at (1014, 350)");

        const auto d2 = GraphDocument::fromXml (makeV1 ({ { "IN", "audioIn", 0.05f, 0.5f }, { "P1", "plugin", 0.5f, 0.5f },
                                                          { "P2", "plugin", 0.5f, 0.5f }, { "P3", "plugin", 0.5f, 0.5f },
                                                          { "OUT", "audioOut", 0.95f, 0.5f } }));
        CHECK (nearPx (d2.getNodeX ("P1"), 324.0f) && nearPx (d2.getNodeX ("P2"), 600.0f) && nearPx (d2.getNodeX ("P3"), 876.0f),
               "stacked plugins are spread left to right (324 / 600 / 876)");
        CHECK (nearPx (d2.getNodeY ("P1"), 350.0f) && nearPx (d2.getNodeY ("P3"), 350.0f), "stacked plugins land on y 350");

        const auto d3 = GraphDocument::fromXml (makeV1 ({ { "IN", "audioIn", 0.05f, 0.5f }, { "P1", "plugin", 0.40f, 0.5f },
                                                          { "P2", "plugin", 0.45f, 0.5f }, { "OUT", "audioOut", 0.95f, 0.5f } }));
        CHECK (noOverlap (d3), "overlapping v1 plugins are separated by the migration");
        CHECK (d3.getNodeX ("IN") < d3.getNodeX ("P1") && d3.getNodeX ("P1") < d3.getNodeX ("P2") && d3.getNodeX ("P2") < d3.getNodeX ("OUT"),
               "migration keeps the left-to-right order");

        CHECK (nearPx (d2.getNodeX ("OUT"), 1014.0f), "R2: the plain v1 default keeps OUT where it was");

        // A tight cluster no scale can clear: stays near its v1 place (R6), OUT is moved clear of the cards (R2).
        const auto d6 = GraphDocument::fromXml (makeV1 ({ { "IN", "audioIn", 0.05f, 0.5f }, { "P1", "plugin", 0.86f, 0.5f },
                                                          { "P2", "plugin", 0.88f, 0.5f }, { "P3", "plugin", 0.90f, 0.5f },
                                                          { "OUT", "audioOut", 0.95f, 0.5f } }));
        CHECK (noOverlap (d6), "R6: a tight cluster is separated");
        CHECK (std::abs (d6.getNodeX ("P1") - 931.0f) < 30.0f, "R6: the first card stays near its v1 x (no 3x blow-up)");
        CHECK (d6.getNodeX ("P1") < d6.getNodeX ("P2") && d6.getNodeX ("P2") < d6.getNodeX ("P3"), "R6: left-to-right order kept");
        {
            using lighthost::geometry::nodeRect;
            const auto outRect = nodeRect (true, { d6.getNodeX ("OUT"), d6.getNodeY ("OUT") });
            bool hidden = false;
            for (auto uid : { "P1", "P2", "P3" })
                hidden = hidden || nodeRect (false, { d6.getNodeX (uid), d6.getNodeY (uid) }).intersects (outRect);
            CHECK (d6.getNodeX ("OUT") > d6.getNodeX ("P3") && ! hidden, "R2: OUT is rightmost and not covered by a card");
        }

        const auto d4 =GraphDocument::fromXml (makeV1 ({ { "IN", "audioIn", 0.05f, 0.5f }, { "OUT", "audioOut", 0.95f, 0.5f } }, ""));
        CHECK (d4.migratedOnLoad && (int) d4.state.getProperty ("version") == 2, "a missing version attribute counts as v1");

        const auto d5 = GraphDocument::fromXml (makeV1 ({ { "IN", "audioIn", 0.05f, 0.5f }, { "OUT", "audioOut", 0.95f, 0.5f } }, "version=\"3\""));
        CHECK (! d5.migratedOnLoad && (int) d5.state.getProperty ("version") == 3, "a newer version is left untouched");
        CHECK (std::abs (d5.getNodeX ("OUT") - 0.95f) < 1.0e-4f, "a newer version keeps its stored values");

        auto tree = d1.state.createCopy();
        CHECK (! GraphDocument::migrateTree (tree), "migrating twice changes nothing");

        GraphDocument v2;
        v2.setNodePosition (v2.getIoNodeUid (true), 123.0f, 456.0f);
        const auto rt = GraphDocument::fromXml (v2.toXml());
        CHECK (! rt.migratedOnLoad && rt.getNodeX (rt.getIoNodeUid (true)) == 123.0f && rt.getNodeY (rt.getIoNodeUid (true)) == 456.0f,
               "a v2 document round-trips its px positions");
    }

    failures += runMeterTests (checks);
    failures += runStripTests (checks);
    failures += runParamTests (checks);
    failures += runRoutingTests (checks);
    failures += runMidiTests (checks);
    failures += runPresetTests (checks);
    failures += runMixTests (checks);
    failures += runSettingsTests (checks);

    std::cout << (failures == 0 ? "PASS " : "FAIL ")
              << (checks - failures) << '/' << checks << " checks\n";
    return failures == 0 ? 0 : 1;
}
