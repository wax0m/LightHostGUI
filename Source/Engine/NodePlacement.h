//
//  NodePlacement.h — where a newly added plugin card goes. Pure and header-only so it is
//  testable on a bare GraphDocument. Existing cards are never moved; only OUT may be pushed
//  right to make room.
//

#pragma once

#include <JuceHeader.h>
#include <optional>
#include "GraphDocument.h"
#include "NodeGeometry.h"

namespace lighthost
{
    struct NewNodePlacement
    {
        juce::Point<float> centre;                       // centre of the new card
        std::optional<juce::Point<float>> moveOutTo;     // new centre for the audioOut node, if it must move
    };

    inline NewNodePlacement placeNewPlugin (const GraphDocument& doc)
    {
        using namespace geometry;

        const juce::String inUid  = doc.getIoNodeUid (true);
        const juce::String outUid = doc.getIoNodeUid (false);
        const auto nodes = doc.getNodes();

        const auto centreOf = [&doc] (const juce::String& uid) -> juce::Point<float>
        {
            return { doc.getNodeX (uid), doc.getNodeY (uid) };
        };

        // 1. Anchor: the node feeding OUT on a serial chain, otherwise the rightmost card (IN if none).
        juce::String anchor = inUid;
        if (doc.isLinearChain())
        {
            const auto conns = doc.getConnections();
            for (int i = 0; i < conns.getNumChildren(); ++i)
                if (conns.getChild (i).getProperty ("dstUid").toString() == outUid)
                {
                    anchor = conns.getChild (i).getProperty ("srcUid").toString();
                    break;
                }
        }
        else
        {
            bool found = false;
            for (int i = 0; i < nodes.getNumChildren(); ++i)
            {
                const auto n = nodes.getChild (i);
                if (n.getProperty ("type").toString() != GraphDocument::typePlugin)
                    continue;
                const juce::String uid = n.getProperty ("uid").toString();
                const auto c = centreOf (uid);
                const auto best = centreOf (anchor);
                if (! found || c.x > best.x || (c.x == best.x && c.y < best.y))
                {
                    anchor = uid;
                    found = true;
                }
            }
        }

        // Every node except OUT, which is the one being (re)placed.
        const auto overlapsAny = [&] (const juce::Rectangle<float>& r)
        {
            const auto grown = r.expanded ((float) nodeGap * 0.5f);
            for (int i = 0; i < nodes.getNumChildren(); ++i)
            {
                const auto n = nodes.getChild (i);
                const juce::String uid = n.getProperty ("uid").toString();
                if (uid == outUid)
                    continue;
                const bool endpoint = n.getProperty ("type").toString() != GraphDocument::typePlugin;
                if (grown.intersects (nodeRect (endpoint, centreOf (uid))))
                    return true;
            }
            return false;
        };

        // 2. One slot right of the anchor; step down past anything it would cover (OUT is moved next).
        auto cand = centreOf (anchor) + juce::Point<float> ((float) nodeSlot, 0.0f);
        for (int guard = 0; guard < 64 && overlapsAny (nodeRect (false, cand)); ++guard)
            cand.y += (float) (cardHeight + nodeGap);

        // 3. OUT sits at least one slot right of the new card, clear of every node including the new one.
        std::optional<juce::Point<float>> moveOut;
        const auto out = centreOf (outUid);
        if (out.x < cand.x + (float) nodeSlot)
        {
            auto o = juce::Point<float> (cand.x + (float) nodeSlot, out.y);
            const auto newCardRect = nodeRect (false, cand).expanded ((float) nodeGap * 0.5f);
            const auto outClear = [&] (juce::Point<float> p)
            {
                const auto r = nodeRect (true, p);
                return ! newCardRect.intersects (r) && ! overlapsAny (r);
            };
            for (int guard = 0; guard < 64 && ! outClear (o); ++guard)
                o.x += (float) nodeSlot;
            moveOut = o;
        }

        // 4. Keep everything on the canvas: top-left >= 0, centre <= kMaxCoord.
        const auto clampCentre = [] (juce::Point<float> p, bool endpoint)
        {
            const auto size = nodeRect (endpoint, { 0.0f, 0.0f });
            return juce::Point<float> (juce::jlimit (size.getWidth()  * 0.5f, kMaxCoord, p.x),
                                       juce::jlimit (size.getHeight() * 0.5f, kMaxCoord, p.y));
        };

        NewNodePlacement result;
        result.centre = clampCentre (cand, false);
        if (moveOut.has_value())
            result.moveOutTo = clampCentre (*moveOut, true);
        return result;
    }
}
