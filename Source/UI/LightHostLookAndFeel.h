//
//  LightHostLookAndFeel.h — dark, Ableton/Bitwig-inspired palette + LookAndFeel.
//
//  Scoped to the main window (set per-component, never as the global default) so
//  it never disturbs the tray menu, native plugin editor windows, or the plugin
//  list. UI-only: it must not pull in engine/model headers.
//

#pragma once

#include <JuceHeader.h>

namespace lighthost::ui
{

class LightHostLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // Palette — one place to retune the whole UI.
    static juce::Colour bg()        { return juce::Colour (0xff1b1b1e); }  // window
    static juce::Colour panel()     { return juce::Colour (0xff232327); }  // meter track / cards
    static juce::Colour outline()   { return juce::Colour (0xff34343a); }
    static juce::Colour text()      { return juce::Colour (0xffe4e4e6); }
    static juce::Colour textDim()   { return juce::Colour (0xff8a8a92); }
    static juce::Colour accent()    { return juce::Colour (0xff4a9eff); }
    static juce::Colour meterLow()  { return juce::Colour (0xff3ec46d); }  // green
    static juce::Colour meterMid()  { return juce::Colour (0xfff2c94c); }  // amber
    static juce::Colour meterHigh() { return juce::Colour (0xffeb5757); }  // red / over

    LightHostLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, bg());
        setColour (juce::DocumentWindow::textColourId,        text());
        setColour (juce::Label::textColourId,                 text());
        setColour (juce::TooltipWindow::backgroundColourId,   panel());
        setColour (juce::TooltipWindow::textColourId,         text());
    }

    juce::Font labelFont (float height) const
    {
        return juce::Font (juce::FontOptions (height));
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LightHostLookAndFeel)
};

} // namespace lighthost::ui
