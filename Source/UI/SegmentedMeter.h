//
//  SegmentedMeter.h — one vertical segmented-LED meter well (design spec
//  §"Node card — I/O meters"). Fed a linear peak on the message-thread timer;
//  paints a bottom-anchored spectrum revealed by level, with a 3px/1px LED gap
//  overlay. View-only: depends on nothing but the LookAndFeel palette.
//

#pragma once

#include <JuceHeader.h>
#include "LightHostLookAndFeel.h"

namespace lighthost::ui
{

class SegmentedMeter : public juce::Component
{
public:
    SegmentedMeter() { setInterceptsMouseClicks (false, false); }

    // Message thread: push a linear peak (0..~2). Applies fast-attack/slow-release
    // ballistics on the normalised-dB level so the LEDs fall smoothly.
    void setLevel (float linearPeak)
    {
        const float target = normDb (linearPeak);
        display = target >= display ? target : display * 0.80f + target * 0.20f;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto well = getLocalBounds().toFloat();

        g.setColour (LightHostLookAndFeel::meterWell());
        g.fillRoundedRectangle (well, 2.0f);

        auto inner = well.reduced (1.0f);

        // Full-height spectrum (green low -> amber -> red top).
        juce::ColourGradient spec (LightHostLookAndFeel::meterGreenLo(), inner.getX(), inner.getBottom(),
                                   LightHostLookAndFeel::meterRed(),     inner.getX(), inner.getY(), false);
        spec.addColour (0.40, LightHostLookAndFeel::meterGreen());
        spec.addColour (0.58, LightHostLookAndFeel::meterGreen());
        spec.addColour (0.80, LightHostLookAndFeel::meterAmber());
        g.setGradientFill (spec);
        g.fillRect (inner);

        // Cover the un-lit top portion with the well colour.
        const float litH = inner.getHeight() * juce::jlimit (0.0f, 1.0f, display);
        g.setColour (LightHostLookAndFeel::meterWell());
        g.fillRect (inner.withHeight (inner.getHeight() - litH));

        // LED gap overlay: 3px lit segment, 1px gap.
        g.setColour (LightHostLookAndFeel::meterWell());
        for (float yy = inner.getY(); yy < inner.getBottom(); yy += 4.0f)
            g.fillRect (inner.getX(), yy + 3.0f, inner.getWidth(), 1.0f);

        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawRoundedRectangle (well, 2.0f, 1.0f);
    }

private:
    static float normDb (float linear)
    {
        const float db = juce::Decibels::gainToDecibels (linear, -60.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);   // -60..+6 dB -> 0..1
    }

    float display = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SegmentedMeter)
};

} // namespace lighthost::ui
