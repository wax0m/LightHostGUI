//
//  RoutingTests.cpp — headless tests for the M6 arbitrary-routing document model:
//  connection validity, cycle rejection, fan-out/fan-in, and node-position
//  persistence. Exposes runRoutingTests(int& checks). No controller / audio needed.
//

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
    doc.setNodePosition (B, 0.42f, 0.66f);
    GraphDocument rt = GraphDocument::fromXml (doc.toXml());
    check (checks, rt.isValid(),                    "routed doc round-trips valid");
    check (checks, near (rt.getNodeX (B), 0.42f),   "node X persists");
    check (checks, near (rt.getNodeY (B), 0.66f),   "node Y persists");
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

    std::cout << (failures == 0 ? "  routing ok" : "  routing FAILED") << std::endl;
    return failures;
}
