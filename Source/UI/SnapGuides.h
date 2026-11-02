//
//  SnapGuides.h — pure alignment-snap maths for dragging a node on the canvas (no UI, no state).
//  The dragged rect is compared, per axis and independently, with the same quantity of every other node
//  (centre Y / top / bottom, centre X / left / right); the nearest one within 8 screen px wins.
//  Everything is in LOGICAL canvas px, so the threshold is divided by the zoom.
//

#pragma once

#include <JuceHeader.h>
#include <array>
#include <optional>
#include <vector>

namespace lighthost::ui::snap
{

constexpr float kThresholdScreenPx = 8.0f;

struct Result
{
    juce::Point<float> offset;          // add to the raw rect to get the snapped one
    std::optional<float> guideX;        // x of the vertical guide line, if the X axis snapped
    std::optional<float> guideY;        // y of the horizontal guide line, if the Y axis snapped
};

namespace detail
{
    // Nearest candidate pair within `threshold`: returns the offset and the target coordinate.
    inline bool nearest (const float (&mine)[3], const std::vector<std::array<float, 3>>& theirs, float threshold,
                         float& offset, float& guide)
    {
        bool found = false;
        float best = threshold;
        for (const auto& t : theirs)
            for (int i = 0; i < 3; ++i)   // same quantity on both sides: centre/centre, start/start, end/end
            {
                const float diff = t[(size_t) i] - mine[i];
                if (std::abs (diff) <= threshold && (! found || std::abs (diff) < best))
                {
                    best   = std::abs (diff);
                    offset = diff;
                    guide  = t[(size_t) i];
                    found  = true;
                }
            }
        return found;
    }
}

// `others` must not contain the dragged node itself. altDown disables snapping and guides.
inline Result computeSnap (juce::Rectangle<float> raw, const std::vector<juce::Rectangle<float>>& others,
                           float zoom, bool altDown)
{
    Result r;
    if (altDown || others.empty() || zoom <= 0.0f)
        return r;

    const float threshold = kThresholdScreenPx / zoom;

    const float mineX[3] = { raw.getCentreX(), raw.getX(), raw.getRight() };
    const float mineY[3] = { raw.getCentreY(), raw.getY(), raw.getBottom() };
    std::vector<std::array<float, 3>> theirsX, theirsY;
    for (const auto& o : others)
    {
        theirsX.push_back ({ o.getCentreX(), o.getX(), o.getRight() });
        theirsY.push_back ({ o.getCentreY(), o.getY(), o.getBottom() });
    }

    float off = 0.0f, guide = 0.0f;
    if (detail::nearest (mineX, theirsX, threshold, off, guide)) { r.offset.x = off; r.guideX = guide; }
    if (detail::nearest (mineY, theirsY, threshold, off, guide)) { r.offset.y = off; r.guideY = guide; }
    return r;
}

} // namespace lighthost::ui::snap
