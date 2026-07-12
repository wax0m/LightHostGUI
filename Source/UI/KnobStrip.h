//
//  KnobStrip.h — one parameter column: tick-dial knob + UPPERCASE label + value
//  readout (design spec §"Node card — knob"). View-only; the NodeCard wires the
//  slider's onValueChange to the controller. THIS pass a knob drives the host
//  channel strip (gain or pan); a plugin-parameter bridge drops in later with no
//  layout change.
//

#pragma once

#include <JuceHeader.h>
#include "LightHostLookAndFeel.h"

namespace lighthost::ui
{

class KnobStrip : public juce::Component
{
public:
    KnobStrip (const juce::String& labelText, double min, double max, double defaultValue)
        : label (labelText)
    {
        LightHostLookAndFeel::applyKnobStyle (slider);
        slider.setRange (min, max);
        slider.setValue (defaultValue, juce::dontSendNotification);
        slider.onValueChange = [this] { if (onValueChange) onValueChange (slider.getValue()); repaint(); };
        addAndMakeVisible (slider);
    }

    // Maps the current slider value to the readout string.
    std::function<juce::String (double)> format;
    // Fired on drag; NodeCard binds this to the controller.
    std::function<void (double)> onValueChange;

    juce::Slider& getSlider() noexcept { return slider; }

    void setValueQuiet (double v) { slider.setValue (v, juce::dontSendNotification); repaint(); }

    // Double-clicking the knob returns it to `v` (fires onValueChange, so the
    // controller/plugin update through the normal path).
    void setDefault (double v) { slider.setDoubleClickReturnValue (true, v); }

    // Re-label / re-range the column (used when a knob is repurposed from the
    // host gain/pan strip to a hosted-plugin parameter).
    void setLabel (const juce::String& t) { label = t; repaint(); }
    void setRange (double lo, double hi)  { slider.setRange (lo, hi); }

    // Override the readout with a plugin-supplied string (its getCurrentValueAsText);
    // clearReadout() reverts to the `format` callback.
    void setReadout (const juce::String& t) { overrideText = t; hasOverride = true; repaint(); }
    void clearReadout() { hasOverride = false; }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromBottom (30);            // label + value area
        const int s = juce::jmin (r.getWidth(), r.getHeight());
        slider.setBounds (r.withSizeKeepingCentre (s, s));
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        auto valueArea = r.removeFromBottom (16);
        auto labelArea = r.removeFromBottom (14);

        g.setColour (LightHostLookAndFeel::textSecond());
        g.setFont (LightHostLookAndFeel::mono (9.0f));
        g.drawText (label.toUpperCase(), labelArea, juce::Justification::centred);

        g.setColour (LightHostLookAndFeel::textPrimary());
        g.setFont (LightHostLookAndFeel::mono (11.0f));
        const juce::String v = hasOverride ? overrideText
                             : format      ? format (slider.getValue())
                                           : juce::String (slider.getValue(), 2);
        g.drawText (v, valueArea, juce::Justification::centred);
    }

private:
    juce::Slider slider;
    juce::String label;
    juce::String overrideText;
    bool hasOverride = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KnobStrip)
};

} // namespace lighthost::ui
