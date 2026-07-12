//
//  LightHostLookAndFeel.h — dark, warm-neutral "tactile" palette + LookAndFeel
//  for the node canvas, recreated from the Claude Design spec
//  (Resources/design-reference/README.md). Accent #d98a4d.
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
    //== Palette (exact hex from the design spec) ============================
    static juce::Colour accent()      { return juce::Colour (0xffd98a4d); }
    static juce::Colour accentDim()   { return juce::Colour (0xff423f3b); }  // knob arc track

    static juce::Colour appBg()       { return juce::Colour (0xff191817); }
    static juce::Colour appBgTop()    { return juce::Colour (0xff282521); }  // radial centre (tactile)
    static juce::Colour appBgBottom() { return juce::Colour (0xff1c1a17); }

    static juce::Colour titleTop()    { return juce::Colour (0xff2f2b26); }
    static juce::Colour titleBottom() { return juce::Colour (0xff242019); }
    static juce::Colour titleBorder() { return juce::Colour (0xff38332d); }

    static juce::Colour cardTop()     { return juce::Colour (0xff332f2b); }
    static juce::Colour cardBottom()  { return juce::Colour (0xff2a2723); }
    static juce::Colour cardBorder()  { return juce::Colour (0xff423e39); }
    static juce::Colour cardEdge()    { return juce::Colour (0xff514b43); }  // top highlight

    static juce::Colour nodeTop()     { return juce::Colour (0xff262119); }
    static juce::Colour nodeBottom()  { return juce::Colour (0xff1a1713); }
    static juce::Colour nodeBorder()  { return juce::Colour (0xff3b3630); }

    static juce::Colour textPrimary() { return juce::Colour (0xffe6e2db); }
    static juce::Colour textSecond()  { return juce::Colour (0xff8f897f); }
    static juce::Colour textTert()    { return juce::Colour (0xff6b665f); }
    static juce::Colour textTert2()   { return juce::Colour (0xff7f7a71); }

    static juce::Colour bypassTrack() { return juce::Colour (0xff2a2724); }
    static juce::Colour bypassThumb() { return juce::Colour (0xffefe9e0); }

    static juce::Colour meterWell()   { return juce::Colour (0xff131110); }
    static juce::Colour meterGreenLo(){ return juce::Colour (0xff2c5f28); }
    static juce::Colour meterGreen()  { return juce::Colour (0xff74b84a); }
    static juce::Colour meterAmber()  { return juce::Colour (0xffd6a13f); }
    static juce::Colour meterRed()    { return juce::Colour (0xffd65a45); }

    static juce::Colour removeHover() { return juce::Colour (0xffe0674f); }
    static juce::Colour connector()   { return juce::Colour (0xff544e46); }

    static juce::Colour menuSurface() { return juce::Colour (0xff2b2824); }
    static juce::Colour menuBorder()  { return juce::Colour (0xff443f37); }
    static juce::Colour menuItem()    { return juce::Colour (0xffdcd6cd); }

    //== Fonts (Archivo / IBM Plex Mono if installed, else graceful fallback) =
    static juce::Font display (float height, bool bold = false)
    {
        return juce::Font (juce::FontOptions ("Archivo", height,
                                              bold ? juce::Font::bold : juce::Font::plain));
    }

    static juce::Font mono (float height)
    {
        auto name = juce::Font::getDefaultMonospacedFontName();
        return juce::Font (juce::FontOptions (name, height, juce::Font::plain));
    }

    //=======================================================================
    LightHostLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, appBg());
        setColour (juce::DocumentWindow::textColourId,        textPrimary());
        setColour (juce::Label::textColourId,                 textPrimary());
        setColour (juce::PopupMenu::backgroundColourId,       menuSurface());
        setColour (juce::PopupMenu::textColourId,             menuItem());
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent().withAlpha (0.16f));
        setColour (juce::PopupMenu::highlightedTextColourId,  textPrimary());
        setColour (juce::TooltipWindow::backgroundColourId,   menuSurface());
        setColour (juce::TooltipWindow::textColourId,         menuItem());
        setColour (juce::ScrollBar::thumbColourId,            juce::Colour (0xff4a453e));
        setColour (juce::ScrollBar::trackColourId,            juce::Colours::transparentBlack);
        setColour (juce::ScrollBar::backgroundColourId,       juce::Colours::transparentBlack);
    }

    //== Tick-dial rotary knob (design spec §"Node card — knob") =============
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float startAngle, float endAngle,
                           juce::Slider&) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
        const float size   = juce::jmin (bounds.getWidth(), bounds.getHeight());
        const auto  centre = bounds.getCentre();
        const float radius = size * 0.5f;
        const float angle  = startAngle + sliderPos * (endAngle - startAngle);

        // 1. Tick ring — neutral radial ticks around the 270° travel.
        {
            const int   numTicks = 11;
            const float tOuter   = radius * 0.99f;
            const float tInner   = radius * 0.84f;
            g.setColour (juce::Colour (0xff8f8579));
            for (int i = 0; i < numTicks; ++i)
            {
                const float a = startAngle + (float) i / (float) (numTicks - 1) * (endAngle - startAngle);
                const juce::Point<float> p1 (centre.x + tInner * std::sin (a), centre.y - tInner * std::cos (a));
                const juce::Point<float> p2 (centre.x + tOuter * std::sin (a), centre.y - tOuter * std::cos (a));
                g.drawLine ({ p1, p2 }, 1.3f);
            }
        }

        // 2. Value arc — 270° band; accent for the filled portion, track for the rest.
        {
            const float arcR = radius * 0.77f;
            const float thick = radius * 0.13f;
            juce::Path track, value;
            track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
            g.setColour (accentDim());
            g.strokePath (track, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, angle, true);
            g.setColour (accent());
            g.strokePath (value, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // 3. Domed body.
        {
            const float bodyR = radius * 0.74f;
            auto body = juce::Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (centre);
            juce::ColourGradient grad (juce::Colour (0xff4a443b), body.getCentreX(), body.getY(),
                                       juce::Colour (0xff24211d), body.getCentreX(), body.getBottom(), false);
            g.setGradientFill (grad);
            g.fillEllipse (body);
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            g.drawEllipse (body, 1.0f);
        }

        // 4. Pointer.
        {
            const float len = radius * 0.62f;
            const float w   = 2.0f;
            juce::Path ptr;
            ptr.addRoundedRectangle (-w * 0.5f, -len, w, len, 1.0f);
            g.setColour (juce::Colour (0xfff0eae1));
            g.fillPath (ptr, juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
        }

        // 5. Centre cap.
        {
            const float capR = radius * 0.16f;
            auto cap = juce::Rectangle<float> (capR * 2.0f, capR * 2.0f).withCentre (centre);
            g.setColour (juce::Colour (0xff1d1a16));
            g.fillEllipse (cap);
        }
    }

    // Rotary travel that matches the spec: −135°…+135°, gap at the bottom.
    static void applyKnobStyle (juce::Slider& s)
    {
        s.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        // JUCE requires both angles >= 0 and < 4*pi. This is the positive-wrapped
        // form of the spec's -135 deg..+135 deg sweep (identical pointer directions),
        // gap at the bottom. Using negative radians trips a jassert in Slider.
        s.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                               juce::MathConstants<float>::pi * 2.75f, true);
        s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        s.setVelocityBasedMode (false);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LightHostLookAndFeel)
};

} // namespace lighthost::ui
