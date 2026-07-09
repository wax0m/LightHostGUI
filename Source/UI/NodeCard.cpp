#include "NodeCard.h"

namespace lighthost::ui
{

NodeCard::NodeCard (GraphController& c, GraphController::ChainItem item)
    : controller (c), uid (item.uid), name (item.name),
      bypassed (item.bypassed), missing (item.missing)
{
    // Format badge: pull from the live plugin instance if present.
    if (auto* node = controller.getNodeForUid (uid))
        if (auto* proc = node->getProcessor())
            if (auto* inst = dynamic_cast<juce::AudioPluginInstance*> (proc))
                format = inst->getPluginDescription().pluginFormatName;
    if (format.isEmpty())
        format = missing ? "missing" : "VST3";

    addAndMakeVisible (knobA);
    addAndMakeVisible (knobB);
    addAndMakeVisible (meterL);
    addAndMakeVisible (meterR);

    // A MISSING plugin has no live instance to drive: show no knobs (just the
    // header + inert meter). Otherwise prefer the hosted plugin's own parameters
    // (Freq, Gain, Ratio, …) on the card, falling back to the host gain/pan strip
    // when the plugin exposes none.
    if (missing)
    {
        knobA.setVisible (false);
        knobB.setVisible (false);
    }
    else if (auto cardParams = controller.getNodeCardParameters (uid, 2); ! cardParams.empty())
        setupPluginParamKnobs (cardParams);
    else
        setupGainPanKnobs();

    applyBypassLook();
    setSize (cardWidth, cardHeight);
}

void NodeCard::setupGainPanKnobs()
{
    knobA.setLabel ("Gain");
    knobA.setRange (0.0, 2.0);
    knobA.clearReadout();
    knobA.setValueQuiet (controller.getNodeGain (uid));
    knobA.format = [] (double v) { return v <= 0.0001 ? juce::String ("-inf")
                                     : juce::Decibels::toString ((float) juce::Decibels::gainToDecibels (v), 1, -60.0f); };
    knobA.onValueChange = [this] (double v) { controller.setNodeGain (uid, (float) v); };

    knobB.setLabel ("Pan");
    knobB.setRange (-1.0, 1.0);
    knobB.clearReadout();
    knobB.setValueQuiet (controller.getNodePan (uid));
    knobB.format = [] (double v)
    {
        const int p = juce::roundToInt (std::abs (v) * 100.0);
        if (p == 0) return juce::String ("C");
        return (v < 0 ? juce::String ("L") : juce::String ("R")) + juce::String (p);
    };
    knobB.onValueChange = [this] (double v) { controller.setNodePan (uid, (float) v); };
    knobB.setVisible (true);
}

void NodeCard::setupPluginParamKnobs (const std::vector<params::ParamInfo>& ps)
{
    KnobStrip* slots[2] = { &knobA, &knobB };
    for (size_t i = 0; i < ps.size() && i < 2; ++i)
    {
        const auto& p = ps[i];
        paramIndices.push_back (p.index);

        auto* k = slots[i];
        k->format = nullptr;                 // readout comes from the plugin's own text
        k->setLabel (p.name);
        k->setRange (0.0, 1.0);              // normalized param value maps straight to the dial
        k->setValueQuiet (p.value);
        k->setReadout (p.text.isNotEmpty() ? p.text : juce::String (p.value, 2));

        const int idx = p.index;
        k->onValueChange = [this, idx] (double v) { controller.setNodeParameter (uid, idx, (float) v); };
    }

    // A plugin exposing a single parameter leaves the second column empty.
    knobB.setVisible (paramIndices.size() > 1);
}

void NodeCard::refreshParamReadouts()
{
    if (paramIndices.empty())       // host gain/pan mode: nothing external to poll
        return;

    // ~10 Hz is plenty for a text readout and avoids re-collecting the plugin's
    // (possibly hundreds of) parameters every animation frame.
    if (++paramPollCounter < 6)
        return;
    paramPollCounter = 0;

    const auto ps = controller.getNodeCardParameters (uid, (int) paramIndices.size());
    KnobStrip* slots[2] = { &knobA, &knobB };
    for (size_t i = 0; i < ps.size() && i < 2; ++i)
    {
        auto* k = slots[i];
        if (k->getSlider().isMouseButtonDown())   // don't fight the user mid-drag
            continue;
        k->setValueQuiet (ps[i].value);
        k->setReadout (ps[i].text.isNotEmpty() ? ps[i].text : juce::String (ps[i].value, 2));
    }
}

void NodeCard::applyBypassLook()
{
    const float a = bypassed ? 0.42f : 1.0f;
    knobA.setAlpha (a);
    knobB.setAlpha (a);
    meterL.setAlpha (a);
    meterR.setAlpha (a);
}

void NodeCard::updateMeters()
{
    if (const MeterTap* tap = controller.getNodeMeter (uid))
    {
        const auto lv = tap->read();
        meterL.setLevel (lv.peak[0]);
        meterR.setLevel (lv.peak[1]);
    }
    else
    {
        meterL.setLevel (0.0f);
        meterR.setLevel (0.0f);
    }

    refreshParamReadouts();
}

void NodeCard::resized()
{
    const int pad = 15;
    auto r = getLocalBounds().reduced (pad);

    auto header = r.removeFromTop (34);
    statusDotRect = header.removeFromLeft (10).withSizeKeepingCentre (8, 8);
    removeRect = header.removeFromRight (19).withSizeKeepingCentre (19, 19);
    header.removeFromRight (6);
    bypassRect = header.removeFromRight (34).withSizeKeepingCentre (34, 16);

    r.removeFromTop (14);   // divider band

    auto meterBlock = r.removeFromBottom (98 + 16);
    {
        auto wells = meterBlock.removeFromTop (98).withSizeKeepingCentre (12 * 2 + 10, 98);
        meterL.setBounds (wells.removeFromLeft (12));
        wells.removeFromLeft (10);
        meterR.setBounds (wells.removeFromLeft (12));
    }

    r.removeFromTop (6);
    if (! knobB.isVisible())        // single-parameter plugin: centre the lone knob
    {
        knobA.setBounds (r);
    }
    else
    {
        const int kgap = 14;
        const int kw = (r.getWidth() - kgap) / 2;
        knobA.setBounds (r.removeFromLeft (kw));
        r.removeFromLeft (kgap);
        knobB.setBounds (r);
    }
}

void NodeCard::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (0.5f);

    // Card surface + border + top edge highlight.
    juce::ColourGradient grad (LightHostLookAndFeel::cardTop(), b.getX(), b.getY(),
                               LightHostLookAndFeel::cardBottom(), b.getX(), b.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (b, 9.0f);
    g.setColour (LightHostLookAndFeel::cardEdge().withAlpha (0.5f));
    g.drawLine (b.getX() + 8, b.getY() + 1.0f, b.getRight() - 8, b.getY() + 1.0f, 1.0f);
    g.setColour (LightHostLookAndFeel::cardBorder());
    g.drawRoundedRectangle (b, 9.0f, 1.0f);

    const float contentAlpha = bypassed ? 0.5f : 1.0f;

    // Status dot.
    if (bypassed)
        g.setColour (juce::Colour (0xff4a453f));
    else
    {
        g.setColour (LightHostLookAndFeel::accent().withAlpha (0.35f));
        g.fillEllipse (statusDotRect.toFloat().expanded (2.0f));
        g.setColour (LightHostLookAndFeel::accent());
    }
    g.fillEllipse (statusDotRect.toFloat());

    // Name + format.
    {
        auto textArea = juce::Rectangle<int> (statusDotRect.getRight() + 8, 12,
                                              bypassRect.getX() - statusDotRect.getRight() - 14, 30);
        g.setColour (LightHostLookAndFeel::textPrimary().withAlpha (contentAlpha));
        g.setFont (LightHostLookAndFeel::display (13.5f, true));
        g.drawText (name, textArea.removeFromTop (18), juce::Justification::bottomLeft);
        g.setColour (LightHostLookAndFeel::textSecond().withAlpha (contentAlpha));
        g.setFont (LightHostLookAndFeel::mono (9.5f));
        g.drawText (format.toUpperCase(), textArea, juce::Justification::topLeft);
    }

    // Bypass switch.
    {
        auto t = bypassRect.toFloat();
        g.setColour (bypassed ? LightHostLookAndFeel::bypassTrack() : LightHostLookAndFeel::accent());
        g.fillRoundedRectangle (t, t.getHeight() * 0.5f);
        const float d = t.getHeight() - 4.0f;
        const float tx = bypassed ? t.getX() + 2.0f : t.getRight() - 2.0f - d;
        g.setColour (LightHostLookAndFeel::bypassThumb());
        g.fillEllipse (tx, t.getY() + 2.0f, d, d);
    }

    // Remove ×.
    {
        auto x = removeRect.toFloat();
        if (removeHot)
        {
            g.setColour (LightHostLookAndFeel::removeHover().withAlpha (0.14f));
            g.fillRoundedRectangle (x, 5.0f);
        }
        g.setColour (removeHot ? LightHostLookAndFeel::removeHover() : LightHostLookAndFeel::textTert());
        g.setFont (LightHostLookAndFeel::display (16.0f));
        g.drawText ("×", removeRect, juce::Justification::centred);
    }

    // Divider.
    {
        const float y = (float) (15 + 34 + 7);
        juce::ColourGradient dg (juce::Colours::transparentBlack, (float) 15, y,
                                 juce::Colours::transparentBlack, (float) (getWidth() - 15), y, false);
        dg.addColour (0.18, juce::Colour (0xff4a453e));
        dg.addColour (0.82, juce::Colour (0xff4a453e));
        g.setGradientFill (dg);
        g.fillRect (15.0f, y, (float) (getWidth() - 30), 1.0f);
    }

    // Meter labels.
    g.setColour (LightHostLookAndFeel::textTert().withAlpha (contentAlpha));
    g.setFont (LightHostLookAndFeel::mono (8.0f));
    g.drawText ("L", meterL.getBounds().withY (meterL.getBottom() + 2).withHeight (12).expanded (6, 0),
                juce::Justification::centred);
    g.drawText ("R", meterR.getBounds().withY (meterR.getBottom() + 2).withHeight (12).expanded (6, 0),
                juce::Justification::centred);
}

void NodeCard::mouseDown (const juce::MouseEvent& e)
{
    if (bypassRect.contains (e.getPosition()))
    {
        bypassed = ! bypassed;
        controller.setBypassed (uid, bypassed);
        applyBypassLook();
        repaint();
        return;
    }
    if (removeRect.contains (e.getPosition()))
    {
        if (onRemove) onRemove (uid);
        return;
    }
}

void NodeCard::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (bypassRect.contains (e.getPosition()) || removeRect.contains (e.getPosition()))
        return;
    if (onOpenEditor) onOpenEditor (uid);
}

} // namespace lighthost::ui
