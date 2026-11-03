//
//  OutputPairs.h — pure helpers for binding output nodes to device channel pairs. Header-only so the
//  rules are testable without a device.
//
//  The audio device hands the graph only its ACTIVE output channels, compacted in ascending order
//  (AudioProcessorPlayer). A binding names a pair by the ABSOLUTE index of its left channel plus the
//  device it was made on, so enabling more channels later can't move a feed that was bound.
//

#pragma once

#include <JuceHeader.h>
#include <optional>

namespace lighthost::outputs
{
    // What the controller knows about the open output device.
    struct Layout
    {
        bool known = false;                 // false until a device has been opened
        juce::String deviceName;
        juce::BigInteger activeMask;        // absolute output channel -> active
        juce::StringArray names;            // absolute output channel names

        bool operator== (const Layout& o) const
        {
            return known == o.known && deviceName == o.deviceName && activeMask == o.activeMask && names == o.names;
        }
        bool operator!= (const Layout& o) const { return ! (*this == o); }
    };

    // Absolute index of the first active output channel, -1 if none.
    inline int firstActiveOutput (const juce::BigInteger& mask)
    {
        return mask.findNextSetBit (0);
    }

    // Position of the pair (outCh, outCh + 1) among the active channels, i.e. the graph output channel
    // its left side lands on. -1 unless BOTH channels of the pair are active.
    inline int activeIndex (const juce::BigInteger& mask, int outCh)
    {
        if (outCh < 0 || ! mask[outCh] || ! mask[outCh + 1])
            return -1;

        int index = 0;
        for (int i = 0; i < outCh; ++i)
            if (mask[i])
                ++index;
        return index;
    }

    // "Loop-back 1/2" from the channel names "Loop-back 1" / "Loop-back 2"; falls back to "Out 3/4".
    inline juce::String pairLabel (const juce::StringArray& names, int outCh)
    {
        const auto fallback = "Out " + juce::String (outCh + 1) + "/" + juce::String (outCh + 2);
        if (outCh < 0 || outCh + 1 >= names.size())
            return fallback;

        const auto left = names[outCh].trim(), right = names[outCh + 1].trim();
        if (left.isEmpty() || right.isEmpty())
            return fallback;

        int common = 0;
        while (common < left.length() && common < right.length() && left[common] == right[common])
            ++common;
        while (common > 0 && juce::CharacterFunctions::isDigit (left[common - 1]))   // never cut a number in half
            --common;

        const auto leftRest = left.substring (common), rightRest = right.substring (common);
        if (common > 0 && leftRest.isNotEmpty() && rightRest.isNotEmpty() && leftRest.length() <= 3 && rightRest.length() <= 3)
            return left.substring (0, common) + leftRest + "/" + rightRest;

        return left + " + " + right;
    }

    // One output node's binding, as stored in the document.
    struct Binding
    {
        bool bound = false;          // has outCh
        int  outCh = 0;
        juce::String outDevice;      // device the binding was made on ("" = any)
        bool isMain = false;
    };

    // Graph output channel the node's pair lands on, or -1 when it must stay unwired (silent).
    //  - bound to THIS device (or to none): strict, by absolute channel. An inactive pair is silent.
    //  - not bound, or bound to ANOTHER device: the main OUT falls back to the first active pair, as
    //    before v1.2; an extra OUT is inactive.
    //  - no device opened yet: the main OUT uses the first pair, an extra OUT is inactive.
    inline int outputBase (const Binding& b, const Layout& layout)
    {
        if (! layout.known)
            return b.isMain ? 0 : -1;

        if (b.bound && (b.outDevice.isEmpty() || b.outDevice == layout.deviceName))
            return activeIndex (layout.activeMask, b.outCh);

        return b.isMain ? 0 : -1;
    }

    // Where "unpinned" outputs get stamped: the first COMPLETE active pair (both channels) of the open device.
    struct PinTarget
    {
        int outCh = 0;
        juce::String label, device;
    };

    inline std::optional<PinTarget> pinTarget (const Layout& layout)
    {
        if (! layout.known)
            return std::nullopt;
        // A pair the graph can't wire (one channel of it inactive) would leave the pinned OUT silent, so the
        // first channel alone isn't enough; with no complete pair nothing is pinned and the main OUT stays
        // unbound (first active channels, as before v1.2).
        for (int ch = firstActiveOutput (layout.activeMask); ch >= 0; ch = layout.activeMask.findNextSetBit (ch + 1))
            if (layout.activeMask[ch + 1])
                return PinTarget { ch, pairLabel (layout.names, ch), layout.deviceName };
        return std::nullopt;
    }
}
