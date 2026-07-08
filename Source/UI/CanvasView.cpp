#include "CanvasView.h"

namespace lighthost::ui
{

//== Endpoint ==================================================================
void EndpointNode::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (0.5f);
    juce::ColourGradient grad (LightHostLookAndFeel::nodeTop(), b.getX(), b.getY(),
                               LightHostLookAndFeel::nodeBottom(), b.getX(), b.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (b, 11.0f);
    g.setColour (LightHostLookAndFeel::nodeBorder());
    g.drawRoundedRectangle (b, 11.0f, 1.0f);

    // Accent dot with glow + label.
    auto centre = b.getCentre();
    auto dot = juce::Rectangle<float> (9, 9).withCentre ({ centre.x, centre.y - 8 });
    g.setColour (LightHostLookAndFeel::accent().withAlpha (0.35f));
    g.fillEllipse (dot.expanded (4.0f));
    g.setColour (LightHostLookAndFeel::accent());
    g.fillEllipse (dot);

    g.setColour (LightHostLookAndFeel::textSecond());
    g.setFont (LightHostLookAndFeel::mono (10.0f));
    g.drawText (text, b.withTop (centre.y + 4).withHeight (16), juce::Justification::centred);
}

//== Connector =================================================================
void Connector::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    const float cy = b.getCentreY();
    const float x0 = b.getX();
    const float x1 = b.getRight() - 8.0f;   // leave room for the arrowhead

    juce::ColourGradient grad (juce::Colour (0xff4d4842), x0, cy,
                               juce::Colour (0xff5c554c), x1, cy, false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (x0, cy - 1.0f, x1 - x0, 2.0f, 1.0f);

    // Accent arrowhead.
    juce::Path tri;
    tri.addTriangle (x1, cy - 6.0f, x1 + 8.0f, cy, x1, cy + 6.0f);
    g.setColour (LightHostLookAndFeel::accent().withAlpha (0.85f));
    g.fillPath (tri);

    // Traveling signal-flow dot.
    const float dx = x0 + phase * (x1 - x0);
    float alpha = 1.0f;
    if (phase < 0.12f) alpha = phase / 0.12f;
    else if (phase > 0.88f) alpha = (1.0f - phase) / 0.12f;
    g.setColour (LightHostLookAndFeel::accent().withAlpha (juce::jlimit (0.0f, 1.0f, alpha)));
    g.fillEllipse (dx - 3.0f, cy - 3.0f, 6.0f, 6.0f);
}

//== Add slot ==================================================================
void AddSlot::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (0.75f);
    juce::Path border;
    border.addRoundedRectangle (b, 11.0f);
    const float dashes[] = { 5.0f, 4.0f };
    g.setColour (hot ? LightHostLookAndFeel::accent().withAlpha (0.7f) : juce::Colour (0xff45403a));
    juce::PathStrokeType (1.5f).createDashedStroke (border, border, dashes, 2);
    g.strokePath (border, juce::PathStrokeType (1.5f));

    auto centre = b.getCentre();
    auto circle = juce::Rectangle<float> (30, 30).withCentre ({ centre.x, centre.y - 8 });
    g.setColour (hot ? LightHostLookAndFeel::accent().withAlpha (0.8f) : juce::Colour (0xff55504a));
    g.drawEllipse (circle, 1.5f);
    g.setColour (hot ? LightHostLookAndFeel::accent() : LightHostLookAndFeel::textSecond());
    g.setFont (LightHostLookAndFeel::display (20.0f));
    g.drawText ("+", circle, juce::Justification::centred);

    g.setColour (LightHostLookAndFeel::textTert());
    g.setFont (LightHostLookAndFeel::mono (8.0f));
    g.drawText ("ADD", b.withTop (centre.y + 12).withHeight (14), juce::Justification::centred);
}

//== CanvasView ================================================================
CanvasView::CanvasView (GraphController& c) : controller (c)
{
    addAndMakeVisible (inNode);
    addAndMakeVisible (outNode);
    addAndMakeVisible (addSlot);
    addSlot.onClick = [this]
    {
        if (onAddPlugin)
            onAddPlugin (addSlot.localPointToGlobal (addSlot.getLocalBounds().getBottomLeft()));
    };
    refresh();
}

void CanvasView::refresh()
{
    cards.clear();
    connectors.clear();

    for (const auto& item : controller.getChain())
    {
        auto* card = new NodeCard (controller, item);
        card->onOpenEditor = [this] (const juce::String& uid) { if (onOpenEditor) onOpenEditor (uid); };
        card->onRemove = [this] (const juce::String& uid)
        {
            // Defer so the card isn't destroyed inside its own mouse callback.
            juce::MessageManager::callAsync ([this, uid]
            {
                controller.removeFromChain (uid);
                refresh();
            });
        };
        addAndMakeVisible (card);
        cards.add (card);
    }

    for (int i = 0; i < cards.size() + 1; ++i)
        connectors.add (new Connector());
    for (auto* con : connectors)
        addAndMakeVisible (con);

    const int pad = 36;
    contentWidth = pad + 54 + cards.size() * (40 + NodeCard::cardWidth) + 40 + 54 + 8 + 66 + pad;
    setSize (contentWidth, canvasHeight);   // fixed height; MainComponent scrolls horizontally
    resized();
}

void CanvasView::tick()
{
    for (auto* card : cards)
        card->updateMeters();

    flowPhase += 1.0f / 96.0f;              // ~1.6 s loop at 60 fps
    if (flowPhase > 1.0f) flowPhase -= 1.0f;
    for (auto* con : connectors)
        con->setPhase (flowPhase);
}

void CanvasView::resized()
{
    const int pad = 36;
    const int cy  = getHeight() / 2;

    int x = pad;
    auto place = [&] (juce::Component& c, int w, int h)
    {
        c.setBounds (x, cy - h / 2, w, h);
        x += w;
    };

    place (inNode, 54, 150);
    for (int i = 0; i < cards.size(); ++i)
    {
        connectors[i]->setBounds (x, cy - 12, 40, 24);  x += 40;
        place (*cards[i], NodeCard::cardWidth, NodeCard::cardHeight);
    }
    connectors.getLast()->setBounds (x, cy - 12, 40, 24); x += 40;
    place (outNode, 54, 150);
    x += 8;
    place (addSlot, 66, 150);

    contentWidth = x + pad;
}

void CanvasView::paint (juce::Graphics& g)
{
    juce::ColourGradient bg (LightHostLookAndFeel::appBgTop(), (float) getWidth() * 0.5f, -40.0f,
                             LightHostLookAndFeel::appBgBottom(), (float) getWidth() * 0.5f, (float) getHeight(), true);
    g.setGradientFill (bg);
    g.fillAll();
}

} // namespace lighthost::ui
