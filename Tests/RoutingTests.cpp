//
//  RoutingTests.cpp — headless tests for the M6 arbitrary-routing document model:
//  connection validity, cycle rejection, fan-out/fan-in, and node-position
//  persistence. Exposes runRoutingTests(int& checks). No controller / audio needed.
//

#include "../Source/Engine/GraphDocument.h"
#include "../Source/Engine/NodePlacement.h"
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
    bool near (float a, float b, float tol = 1.0e-3f) { return std::abs (a - b) <= tol; }

    PluginDescription fx (const String& name)
    {
        PluginDescription d; d.name = name; d.pluginFormatName = "VST3"; return d;
    }
}

int runRoutingTests (int& checks)
{
    failures = 0;
    std::cout << "Routing tests" << std::endl;

    GraphDocument doc;
    const String in  = doc.getIoNodeUid (true);
    const String out = doc.getIoNodeUid (false);
    const String A = doc.addPluginNode (fx ("A"), 0.3f, 0.5f);
    const String B = doc.addPluginNode (fx ("B"), 0.5f, 0.5f);
    const String C = doc.addPluginNode (fx ("C"), 0.7f, 0.5f);

    // --- basic validity ----------------------------------------------------
    check (checks,   doc.canAddConnection (in, 0, A, 0), "in -> A is allowed");
    check (checks, ! doc.canAddConnection (A, 0, A, 0),  "self-connection rejected");
    check (checks, ! doc.canAddConnection (A, 0, in, 0), "edge INTO audioIn rejected");
    check (checks, ! doc.canAddConnection (out, 0, A, 0),"edge OUT OF audioOut rejected");
    check (checks, ! doc.canAddConnection (in, 2, A, 0), "out-of-range channel rejected");

    // --- duplicate ---------------------------------------------------------
    doc.addConnection (in, 0, A, 0);
    check (checks,   doc.hasConnection (in, 0, A, 0),    "connection recorded");
    check (checks, ! doc.canAddConnection (in, 0, A, 0), "duplicate connection rejected");

    // --- cycles ------------------------------------------------------------
    doc.addConnection (A, 0, B, 0);
    check (checks, ! doc.canAddConnection (B, 0, A, 0),  "direct cycle B->A rejected (A->B exists)");
    doc.addConnection (B, 0, C, 0);
    check (checks, ! doc.canAddConnection (C, 0, A, 0),  "transitive cycle C->A rejected (A->B->C)");
    check (checks,   doc.canAddConnection (C, 0, out, 0),"C -> out (no cycle) allowed");

    // --- fan-out / fan-in (parallel branches) ------------------------------
    check (checks, doc.canAddConnection (in, 0, B, 0),   "fan-out: in -> B also allowed (parallel)");
    doc.addConnection (A, 0, out, 0);
    check (checks, doc.canAddConnection (C, 0, out, 0),  "fan-in: C -> out allowed alongside A -> out");

    // --- removeConnection is specific --------------------------------------
    const int before = doc.getConnections().getNumChildren();
    doc.removeConnection (in, 0, A, 0);
    check (checks, ! doc.hasConnection (in, 0, A, 0),        "specific connection removed");
    check (checks, doc.getConnections().getNumChildren() == before - 1, "only one connection removed");
    check (checks, doc.canAddConnection (in, 0, A, 0),        "removed edge can be re-added");

    // --- node position persists through XML --------------------------------
    doc.setNodePosition (B, 420.0f, 660.0f);
    GraphDocument rt = GraphDocument::fromXml (doc.toXml());
    check (checks, rt.isValid(),                    "routed doc round-trips valid");
    check (checks, near (rt.getNodeX (B), 420.0f),  "node X persists");
    check (checks, near (rt.getNodeY (B), 660.0f),  "node Y persists");
    check (checks, ! rt.migratedOnLoad,             "a v2 document is not migrated again");
    check (checks, rt.hasConnection (A, 0, B, 0),   "arbitrary connections survive round-trip");

    // --- isLinearChain: gates the tray serial-chain ops ---------------------
    {
        GraphDocument d;
        const String i = d.getIoNodeUid (true), o = d.getIoNodeUid (false);
        check (checks, d.isLinearChain(), "empty document is a (trivially) linear chain");

        const String p1 = d.addPluginNode (fx ("P1"), 0.3f, 0.5f);
        const String p2 = d.addPluginNode (fx ("P2"), 0.6f, 0.5f);
        d.addConnection (i, 0, p1, 0);  d.addConnection (i, 1, p1, 1);
        d.addConnection (p1, 0, p2, 0); d.addConnection (p1, 1, p2, 1);
        d.addConnection (p2, 0, o, 0);  d.addConnection (p2, 1, o, 1);
        check (checks, d.isLinearChain(), "in->P1->P2->out (stereo) is linear");

        // A parallel branch (fan-out from in) breaks linearity.
        d.addConnection (i, 0, p2, 0);
        check (checks, ! d.isLinearChain(), "fan-out (parallel branch) is NOT linear");
        d.removeConnection (i, 0, p2, 0);
        check (checks, d.isLinearChain(), "removing the branch restores linearity");

        // A side edge that skips ahead also breaks it.
        d.addConnection (p1, 0, o, 0);
        check (checks, ! d.isLinearChain(), "extra P1->out side edge is NOT linear");
    }

    // --- placeNewPlugin: a new card goes between the last card and OUT -----
    {
        using lighthost::geometry::nodeSlot;
        using lighthost::geometry::nodeRect;

        // Serial chain wiring, the way GraphController::appendToChain rewrites it.
        const auto wireChain = [] (GraphDocument& d, const std::vector<String>& chain)
        {
            d.clearConnections();
            for (size_t i = 0; i + 1 < chain.size(); ++i)
            {
                d.addConnection (chain[i], 0, chain[i + 1], 0);
                d.addConnection (chain[i], 1, chain[i + 1], 1);
            }
        };
        const auto appendSerial = [&] (GraphDocument& d, std::vector<String>& chain, const String& name)
        {
            const auto p = lighthost::placeNewPlugin (d);
            const String uid = d.addPluginNode (fx (name), p.centre.x, p.centre.y);
            if (p.moveOutTo.has_value())
                d.setNodePosition (d.getIoNodeUid (false), p.moveOutTo->x, p.moveOutTo->y);
            chain.insert (chain.end() - 1, uid);
            wireChain (d, chain);
            return uid;
        };
        const auto anyOverlap = [] (const GraphDocument& d)
        {
            std::vector<Rectangle<float>> rects;
            auto nodes = d.getNodes();
            for (int i = 0; i < nodes.getNumChildren(); ++i)
            {
                const auto n = nodes.getChild (i);
                const String uid = n.getProperty ("uid").toString();
                rects.push_back (nodeRect (n.getProperty ("type").toString() != "plugin", { d.getNodeX (uid), d.getNodeY (uid) }));
            }
            for (size_t a = 0; a < rects.size(); ++a)
                for (size_t b = a + 1; b < rects.size(); ++b)
                    if (rects[a].intersects (rects[b]))
                        return true;
            return false;
        };

        // Six serial adds.
        {
            GraphDocument d;
            std::vector<String> chain { d.getIoNodeUid (true), d.getIoNodeUid (false) };
            wireChain (d, chain);
            bool earlierUnchanged = true, stepsOk = true, outRightmost = true;
            float prevX = d.getNodeX (d.getIoNodeUid (true));
            for (int i = 0; i < 6; ++i)
            {
                std::vector<std::pair<String, Point<float>>> prior;
                for (size_t k = 0; k + 1 < chain.size(); ++k)
                    prior.push_back ({ chain[k], { d.getNodeX (chain[k]), d.getNodeY (chain[k]) } });

                const String uid = appendSerial (d, chain, "S" + String (i));

                for (const auto& b : prior)
                    earlierUnchanged = earlierUnchanged && d.getNodeX (b.first) == b.second.x && d.getNodeY (b.first) == b.second.y;
                stepsOk = stepsOk && near (d.getNodeX (uid), prevX + (float) nodeSlot);
                prevX = d.getNodeX (uid);
                outRightmost = outRightmost && d.getNodeX (d.getIoNodeUid (false)) > prevX;
            }
            check (checks, ! anyOverlap (d),     "placement: six serial adds never overlap");
            check (checks, earlierUnchanged,     "placement: an add never moves an existing card");
            check (checks, stepsOk,              "placement: each new card sits one slot right of the previous one");
            check (checks, outRightmost,         "placement: OUT stays right of the newest card");
        }

        // The last card dragged down: the next card follows its row.
        {
            GraphDocument d;
            std::vector<String> chain { d.getIoNodeUid (true), d.getIoNodeUid (false) };
            const String a = appendSerial (d, chain, "A");
            d.setNodePosition (a, d.getNodeX (a), 900.0f);
            const String b = appendSerial (d, chain, "B");
            check (checks, near (d.getNodeY (b), 900.0f) && near (d.getNodeX (b), d.getNodeX (a) + (float) nodeSlot),
                   "placement: the next card follows the row of the last card");
        }

        // A card already sitting on the candidate spot: the new card steps down.
        {
            GraphDocument d;
            const String in2 = d.getIoNodeUid (true), out2 = d.getIoNodeUid (false);
            const String a = d.addPluginNode (fx ("A"), 400.0f, 260.0f);
            const String blocker = d.addPluginNode (fx ("Blocker"), 400.0f + (float) nodeSlot, 260.0f);
            wireChain (d, { in2, blocker, a, out2 });   // A feeds OUT, so A is the anchor
            const auto p = lighthost::placeNewPlugin (d);
            const String c = d.addPluginNode (fx ("C"), p.centre.x, p.centre.y);
            check (checks, near (d.getNodeX (c), d.getNodeX (blocker)) && d.getNodeY (c) > d.getNodeY (blocker) + 100.0f,
                   "placement: an occupied candidate spot steps down");
            if (p.moveOutTo.has_value())
                d.setNodePosition (out2, p.moveOutTo->x, p.moveOutTo->y);
            bool cardsClear = true;
            for (const auto& u : { a, blocker })
                cardsClear = cardsClear && ! nodeRect (false, { d.getNodeX (c), d.getNodeY (c) })
                                                 .intersects (nodeRect (false, { d.getNodeX (u), d.getNodeY (u) }));
            check (checks, cardsClear, "placement: the stepped-down card overlaps no card");
        }

        // Branched graph: the anchor is the rightmost card.
        {
            GraphDocument d;
            const String in2 = d.getIoNodeUid (true), out2 = d.getIoNodeUid (false);
            const String a = d.addPluginNode (fx ("A"), 400.0f, 260.0f);
            const String b = d.addPluginNode (fx ("B"), 1000.0f, 500.0f);
            wireChain (d, { in2, a, out2 });
            d.addConnection (in2, 0, b, 0);
            check (checks, ! d.isLinearChain(), "placement: test graph is branched");
            const auto p = lighthost::placeNewPlugin (d);
            check (checks, near (p.centre.x, 1000.0f + (float) nodeSlot) && near (p.centre.y, 500.0f),
                   "placement: a branched graph anchors on the rightmost card");
        }

        // OUT already far right: it does not move.
        {
            GraphDocument d;
            d.setNodePosition (d.getIoNodeUid (false), 5000.0f, 260.0f);
            check (checks, ! lighthost::placeNewPlugin (d).moveOutTo.has_value(), "placement: a distant OUT is left alone");
        }
    }

    std::cout << (failures == 0 ? "  routing ok" : "  routing FAILED") << std::endl;
    return failures;
}
