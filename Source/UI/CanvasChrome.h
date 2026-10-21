//
//  CanvasChrome.h — the pieces around the node canvas that must NOT scroll with it:
//
//    CanvasZoomHolder   sits between the Viewport and the CanvasView. The canvas keeps
//                       working in logical (unzoomed) coordinates; the holder scales it
//                       with a transform and sizes itself to the scaled extent, so the
//                       viewport's scroll range and scrollbars stay exact at any zoom.
//    FloatingAddButton  the "+ ADD" button, pinned to the viewport's top-left corner.
//    ZoomControls       the "- 100% + Fit" pill, pinned to the bottom-right corner.
//
//  All three are owned by MainComponent; the two buttons are siblings painted above
//  the viewport. Header-only.
//

#pragma once

#include <JuceHeader.h>
#include "LightHostLookAndFeel.h"

namespace lighthost::ui
{

class CanvasZoomHolder : public juce::Component
{
public:
    explicit CanvasZoomHolder (juce::Component& contentToScale) : content (contentToScale)
    {
        setInterceptsMouseClicks (false, true);   // only the canvas itself takes clicks
        addAndMakeVisible (content);
        content.setTopLeftPosition (0, 0);
        updateSize();   // the canvas may already be sized; childBoundsChanged won't fire for that
    }

    void setZoom (float newZoom)
    {
        zoom = newZoom;
        content.setTransform (juce::AffineTransform::scale (zoom));
        updateSize();
    }

    float getZoom() const noexcept { return zoom; }

    void childBoundsChanged (juce::Component* child) override
    {
        if (child == &content)
            updateSize();
    }

private:
    void updateSize()
    {
        setSize (juce::roundToInt ((float) content.getWidth()  * zoom),
                 juce::roundToInt ((float) content.getHeight() * zoom));
    }

    juce::Component& content;
    float zoom = 1.0f;

    JUCE_DECLARE_NON_COPYABLE (CanvasZoomHolder)
};

//==============================================================================
class FloatingAddButton : public juce::Component
{
public:
    FloatingAddButton() = default;

    std::function<void()> onClick;

    void mouseEnter (const juce::MouseEvent&) override { hot = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hot = false; repaint(); }
    void mouseDown  (const juce::MouseEvent& e) override
    {
        if (e.mods.isLeftButtonDown() && onClick)
            onClick();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.75f);

        // Opaque-ish body: it floats over cards and cables, which must not show through.
        g.setColour (LightHostLookAndFeel::nodeTop().withAlpha (0.94f));
        g.fillRoundedRectangle (b, 11.0f);

        juce::Path border;
        border.addRoundedRectangle (b, 11.0f);
        const float dashes[] = { 5.0f, 4.0f };
        g.setColour (hot ? LightHostLookAndFeel::accent().withAlpha (0.7f) : juce::Colour (0xff45403a));
        juce::PathStrokeType (1.5f).createDashedStroke (border, border, dashes, 2);
        g.strokePath (border, juce::PathStrokeType (1.5f));

        auto centre = b.getCentre();
        auto circle = juce::Rectangle<float> (26, 26).withCentre ({ centre.x, centre.y - 6 });
        g.setColour (hot ? LightHostLookAndFeel::accent().withAlpha (0.8f) : juce::Colour (0xff55504a));
        g.drawEllipse (circle, 1.5f);
        g.setColour (hot ? LightHostLookAndFeel::accent() : LightHostLookAndFeel::textSecond());
        g.setFont (LightHostLookAndFeel::display (18.0f));
        g.drawText ("+", circle, juce::Justification::centred);

        g.setColour (LightHostLookAndFeel::textTert());
        g.setFont (LightHostLookAndFeel::mono (8.0f));
        g.drawText ("ADD", b.withTop (centre.y + 10).withHeight (12), juce::Justification::centred);
    }

private:
    bool hot = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FloatingAddButton)
};

//==============================================================================
class ZoomControls : public juce::Component
{
public:
    std::function<void()> onZoomOut, onZoomIn, onReset, onFit;

    static constexpr int preferredWidth  = 140;
    static constexpr int preferredHeight = 26;

    ZoomControls() { setZoom (1.0f); }

    void setZoom (float z)
    {
        const auto text = juce::String (juce::roundToInt (z * 100.0f)) + "%";
        if (text != zoomText)
        {
            zoomText = text;
            repaint();
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        segs[0] = r.removeFromLeft (28);   // -
        segs[1] = r.removeFromLeft (50);   // 100%  (click = reset)
        segs[2] = r.removeFromLeft (28);   // +
        segs[3] = r;                       // Fit
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (LightHostLookAndFeel::nodeTop().withAlpha (0.94f));
        g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
        g.setColour (LightHostLookAndFeel::nodeBorder());
        g.drawRoundedRectangle (b, b.getHeight() * 0.5f, 1.0f);

        const juce::String labels[4] = { juce::String::fromUTF8 ("\xe2\x88\x92"), zoomText, "+", "Fit" };
        for (int i = 0; i < 4; ++i)
        {
            const bool isHot = i == hotSeg;
            g.setColour (isHot ? LightHostLookAndFeel::accent() : LightHostLookAndFeel::textSecond());
            g.setFont (i == 1 || i == 3 ? LightHostLookAndFeel::mono (10.0f) : LightHostLookAndFeel::display (15.0f));
            g.drawText (labels[i], segs[i], juce::Justification::centred);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override  { setHot (segmentAt (e.getPosition())); }
    void mouseExit (const juce::MouseEvent&) override     { setHot (-1); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isLeftButtonDown())
            return;
        switch (segmentAt (e.getPosition()))
        {
            case 0: if (onZoomOut) onZoomOut(); break;
            case 1: if (onReset)   onReset();   break;
            case 2: if (onZoomIn)  onZoomIn();  break;
            case 3: if (onFit)     onFit();     break;
            default: break;
        }
    }

private:
    int segmentAt (juce::Point<int> p) const
    {
        for (int i = 0; i < 4; ++i)
            if (segs[i].contains (p))
                return i;
        return -1;
    }

    void setHot (int s)
    {
        if (s != hotSeg)
        {
            hotSeg = s;
            repaint();
        }
    }

    juce::Rectangle<int> segs[4];
    juce::String zoomText;
    int hotSeg = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ZoomControls)
};

} // namespace lighthost::ui
