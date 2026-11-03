//
//  SnapGuidesTests.cpp — headless tests for the canvas alignment-snap maths (SnapGuides.h).
//  Exposes runSnapGuidesTests(int& checks).
//

#include "../Source/UI/SnapGuides.h"
#include <iostream>

using namespace juce;
namespace sn = lighthost::ui::snap;

namespace
{
    int failures = 0;
    void check (int& checks, bool cond, const String& msg)
    {
        ++checks;
        if (! cond) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; }
    }

    bool near (float a, float b) { return std::abs (a - b) < 0.001f; }

    using R = Rectangle<float>;
}

int runSnapGuidesTests (int& checks)
{
    failures = 0;
    std::cout << "Snap guide tests" << std::endl;

    const R other (400.0f, 100.0f, 200.0f, 120.0f);   // x 400..600, y 100..220, centre (500, 160)

    // Y axis: centre Y within the threshold -> straight cable.
    {
        const auto r = sn::computeSnap (R (50.0f, 107.0f, 100.0f, 106.0f), { other }, 1.0f, false);   // centre Y 160 + ... = 160
        check (checks, r.guideY.has_value() && near (*r.guideY, 160.0f), "centre Y snaps to the other centre Y");
        check (checks, near (R (50.0f, 107.0f, 100.0f, 106.0f).translated (0.0f, r.offset.y).getCentreY(), 160.0f), "snapped centre Y equals the other's (equal port heights)");
    }
    {
        const auto r = sn::computeSnap (R (50.0f, 100.0f, 100.0f, 100.0f), { other }, 1.0f, false);   // centre Y 150, off by 10 -> top 100 == 100 wins
        check (checks, r.guideY.has_value() && near (*r.guideY, 100.0f) && near (r.offset.y, 0.0f), "top edge snaps to the other's top");
    }

    // X axis: centre X, left and right.
    {
        const auto r = sn::computeSnap (R (395.0f, 500.0f, 200.0f, 50.0f), { other }, 1.0f, false);   // left 395 -> 400 (5), right 595 -> 600 (5), centre 495 -> 500 (5)
        check (checks, r.guideX.has_value() && near (r.offset.x, 5.0f), "X axis snaps");
        check (checks, ! r.guideY.has_value() && near (r.offset.y, 0.0f), "Y stays free when nothing is near");
    }
    {
        const auto r = sn::computeSnap (R (503.0f - 40.0f, 500.0f, 80.0f, 50.0f), { other }, 1.0f, false);   // centre X 503 -> 500
        check (checks, r.guideX.has_value() && near (*r.guideX, 500.0f) && near (r.offset.x, -3.0f), "centre X snaps");
    }
    {
        const auto r = sn::computeSnap (R (545.0f, 500.0f, 60.0f, 50.0f), { other }, 1.0f, false);   // right 605 -> 600
        check (checks, r.guideX.has_value() && near (*r.guideX, 600.0f) && near (r.offset.x, -5.0f), "right edge snaps to the other's right");
    }

    // Both axes snap independently.
    {
        const auto r = sn::computeSnap (R (397.0f, 103.0f, 200.0f, 120.0f), { other }, 1.0f, false);
        check (checks, near (r.offset.x, 3.0f) && near (r.offset.y, -3.0f) && r.guideX && r.guideY, "both axes snap independently");
    }

    // Threshold: 8 px snaps, just over does not.
    {
        const auto in  = sn::computeSnap (R (392.0f, 500.0f, 10.0f, 10.0f), { other }, 1.0f, false);   // left 392 -> 8 away
        const auto out = sn::computeSnap (R (391.9f, 500.0f, 10.0f, 10.0f), { other }, 1.0f, false);   // 8.1 away
        check (checks, in.guideX.has_value(), "8 px snaps");
        check (checks, ! out.guideX.has_value() && near (out.offset.x, 0.0f), "8.1 px does not snap");
    }

    // Nearest candidate wins, across nodes and across edge kinds.
    {
        const R a (400.0f, 0.0f, 100.0f, 50.0f), b (403.0f, 300.0f, 100.0f, 50.0f);
        const auto r = sn::computeSnap (R (405.0f, 600.0f, 100.0f, 50.0f), { a, b }, 1.0f, false);   // left: 5 from a, 2 from b
        check (checks, near (r.offset.x, -2.0f) && r.guideX.has_value(), "nearest of several candidates wins");
    }

    // A rect never snaps to itself (the caller leaves it out), and an empty list means no snap.
    {
        const R self (10.0f, 10.0f, 50.0f, 50.0f);
        const auto r = sn::computeSnap (self.translated (3.0f, 3.0f), {}, 1.0f, false);
        check (checks, ! r.guideX && ! r.guideY && near (r.offset.x, 0.0f) && near (r.offset.y, 0.0f), "no others: no snap (ignores itself)");
    }

    // The threshold is screen px: divided by the zoom.
    {
        const auto z2in  = sn::computeSnap (R (396.0f, 500.0f, 10.0f, 10.0f), { other }, 2.0f, false);   // 4 away at zoom 2 -> ok
        const auto z2out = sn::computeSnap (R (395.0f, 500.0f, 10.0f, 10.0f), { other }, 2.0f, false);   // 5 away at zoom 2 -> no
        const auto z05   = sn::computeSnap (R (388.0f, 500.0f, 10.0f, 10.0f), { other }, 0.5f, false);   // 12 away at zoom 0.5 (limit 16)
        check (checks, z2in.guideX.has_value(), "zoom 2: 4 logical px snaps");
        check (checks, ! z2out.guideX.has_value(), "zoom 2: 5 logical px does not snap");
        check (checks, z05.guideX.has_value() && near (z05.offset.x, 12.0f), "zoom 0.5: 12 logical px snaps");
    }

    // Alt = no snap, no guides.
    {
        const auto r = sn::computeSnap (R (397.0f, 103.0f, 200.0f, 120.0f), { other }, 1.0f, true);
        check (checks, ! r.guideX && ! r.guideY && near (r.offset.x, 0.0f) && near (r.offset.y, 0.0f), "Alt disables snap and guides");
    }

    return failures;
}
