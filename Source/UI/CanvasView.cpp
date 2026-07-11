#include "CanvasView.h"

namespace lighthost::ui
{

// Palette for the wiring layer (kept local; reuses the app accent for flow).
static const juce::Colour kCableIdle   { 0xff6a6058 };
static const juce::Colour kPortIdle    { 0xff8a8078 };
static const juce::Colour kValidGreen  { 0xff5ec27a };
static const juce::Colour kInvalidRed  { 0xffd25b5b };

static constexpr float kOutPortHit = 12.0f;   // start a wire from an output port
static constexpr float kInPortHit  = 18.0f;   // drop tolerance onto an input port
static constexpr float kCableHit   = 6.0f;    // click-to-delete tolerance
static constexpr float kPortGap    = 8.0f;    // port dot offset outside the card edge

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

void EndpointNode::mouseDown (const juce::MouseEvent& e)
{
    nodeDragActive = true;
    lastParentPos  = e.getEventRelativeTo (getParentComponent()).getPosition();
}

void EndpointNode::mouseDrag (const juce::MouseEvent& e)
{
    if (! nodeDragActive)
        return;
    const auto p = e.getEventRelativeTo (getParentComponent()).getPosition();
    const auto delta = p - lastParentPos;
    lastParentPos = p;
    if (onDragMove) onDragMove (uid, delta);
}

void EndpointNode::mouseUp (const juce::MouseEvent&)
{
    if (nodeDragActive)
    {
        nodeDragActive = false;
        if (onDragEnd) onDragEnd (uid);
    }
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

//== CanvasView ================================================================
CanvasView::CanvasView (GraphController& c) : controller (c)
{
    inNode.setSize (54, 150);
    outNode.setSize (54, 150);
    addAndMakeVisible (inNode);
    addAndMakeVisible (outNode);
    addAndMakeVisible (addSlot);

    auto wireEndpoint = [this] (EndpointNode& n)
    {
        n.onDragMove = [this] (const juce::String& uid, juce::Point<int> d) { moveNodeBy (uid, d); };
        n.onDragEnd  = [this] (const juce::String& uid) { persistNodePosition (uid); };
    };
    wireEndpoint (inNode);
    wireEndpoint (outNode);

    addSlot.onClick = [this]
    {
        if (onAddPlugin)
            onAddPlugin (addSlot.localPointToGlobal (addSlot.getLocalBounds().getBottomLeft()));
    };

    refresh();
}

void CanvasView::rebuildViews()
{
    cards.clear();
    views.clear();

    inNode.uid  = controller.document.getIoNodeUid (true);
    outNode.uid = controller.document.getIoNodeUid (false);

    auto nodes = controller.document.getNodes();
    for (int i = 0; i < nodes.getNumChildren(); ++i)
    {
        const auto n    = nodes.getChild (i);
        const juce::String uid  = n.getProperty ("uid").toString();
        const juce::String type = n.getProperty ("type").toString();

        if (type == GraphDocument::typeAudioIn)
        {
            views.push_back ({ uid, &inNode, /*hasInput*/ false, /*hasOutput*/ true });
        }
        else if (type == GraphDocument::typeAudioOut)
        {
            views.push_back ({ uid, &outNode, /*hasInput*/ true, /*hasOutput*/ false });
        }
        else
        {
            GraphController::ChainItem item;
            item.uid      = uid;
            item.name     = n.getProperty ("name").toString();
            item.bypassed = controller.document.isBypassed (uid);
            item.missing  = controller.getNodeForUid (uid) == nullptr;

            auto* card = new NodeCard (controller, item);
            card->onOpenEditor = [this] (const juce::String& u) { if (onOpenEditor) onOpenEditor (u); };
            card->onRemove = [this] (const juce::String& u)
            {
                // Defer so the card isn't destroyed inside its own mouse callback.
                juce::MessageManager::callAsync ([this, u]
                {
                    controller.removeFromChain (u);
                    refresh();
                });
            };
            card->onDragMove = [this] (const juce::String& u, juce::Point<int> d) { moveNodeBy (u, d); };
            card->onDragEnd  = [this] (const juce::String& u) { persistNodePosition (u); };
            addAndMakeVisible (card);
            cards.add (card);
            views.push_back ({ uid, card, true, true });
        }
    }

    autoLayoutIfStacked();
}

void CanvasView::autoLayoutIfStacked()
{
    // Serially-built / migrated chains place every plugin at (0.5, 0.5) so they
    // stack. If two or more plugin nodes are still at that default, spread them
    // left→right in document order and persist, giving a sane first layout. Once a
    // node has a real (dragged) position it is left untouched.
    std::vector<juce::String> pluginUids;
    int defaulted = 0;
    for (const auto& v : views)
    {
        if (v.comp == &inNode || v.comp == &outNode)
            continue;
        pluginUids.push_back (v.uid);
        const float x = controller.getNodeX (v.uid);
        const float y = controller.getNodeY (v.uid);
        if (std::abs (x - 0.5f) < 1.0e-3f && std::abs (y - 0.5f) < 1.0e-3f)
            ++defaulted;
    }

    if (defaulted < 2)
        return;

    const int n = (int) pluginUids.size();
    for (int i = 0; i < n; ++i)
    {
        const float nx = n == 1 ? 0.5f
                                : layoutBandLo + (float) i * (layoutBandSpan / (float) (n - 1));
        controller.setNodePosition (pluginUids[(size_t) i], nx, 0.5f);
    }
}

void CanvasView::refresh()
{
    rebuildViews();

    // Scale the virtual width to the plugin count so a migrated multi-plugin chain
    // lays out without overlap: plugins span the normalised layoutBand, so the usable
    // width must be large enough that band-span / (n-1) maps to at least nodeSlot px.
    const int pluginCount = cards.size();
    const int spanPx = pluginCount > 1
        ? juce::roundToInt ((float) nodeSlot * (float) (pluginCount - 1) / layoutBandSpan)
        : 0;
    virtualW = juce::jmax (1200, marginX * 2 + spanPx);

    // TODO(zoom): apply a canvas AffineTransform::scale here for user zoom.
    applyCanvasSize();              // virtual canvas; the viewport scrolls
    repaint();
}

void CanvasView::setVisibleArea (int w, int h)
{
    if (w == visibleW && h == visibleH)
        return;
    visibleW = w;
    visibleH = h;
    applyCanvasSize();
}

void CanvasView::applyCanvasSize()
{
    // Grow to fill the viewport so there is no empty gap, but never below the node
    // content extent; node centres still map onto virtualW/virtualH via centreForNorm.
    setSize (juce::jmax (virtualW, visibleW), juce::jmax (virtualH, visibleH));
    resized();
}

void CanvasView::tick()
{
    for (auto* card : cards)
        card->updateMeters();

    flowPhase += 1.0f / 96.0f;      // ~1.6 s loop at 60 fps
    if (flowPhase > 1.0f) flowPhase -= 1.0f;
    repaint();                      // animate the traveling flow dots
}

//== Geometry ==================================================================
// Node positions map over the ACTUAL canvas size (getWidth()/getHeight()), not the
// virtualW/virtualH content-minimum: applyCanvasSize() grows the component to fill the
// viewport, so mapping over the real size lets nodes reach the whole filled area (no
// dead gap on the right/bottom). getWidth() == jmax(virtualW, visibleW), so this also
// handles window grow/shrink, and place<->persist stay consistent (both read live size).
juce::Point<int> CanvasView::centreForNorm (float nx, float ny) const
{
    const int x = marginX + juce::roundToInt (juce::jlimit (0.0f, 1.0f, nx) * (float) (getWidth()  - 2 * marginX));
    const int y = marginY + juce::roundToInt (juce::jlimit (0.0f, 1.0f, ny) * (float) (getHeight() - 2 * marginY));
    return { x, y };
}

juce::Point<float> CanvasView::normForCentre (juce::Point<int> centre) const
{
    const float nx = (float) (centre.x - marginX) / (float) (getWidth()  - 2 * marginX);
    const float ny = (float) (centre.y - marginY) / (float) (getHeight() - 2 * marginY);
    return { juce::jlimit (0.0f, 1.0f, nx), juce::jlimit (0.0f, 1.0f, ny) };
}

void CanvasView::placeNode (juce::Component& c, const juce::String& uid)
{
    const auto centre = centreForNorm (controller.getNodeX (uid), controller.getNodeY (uid));
    c.setBounds (juce::Rectangle<int> (c.getWidth(), c.getHeight()).withCentre (centre));
}

void CanvasView::moveNodeBy (const juce::String& uid, juce::Point<int> delta)
{
    if (const auto* v = viewForUid (uid))
    {
        auto* c = v->comp;
        int x = juce::jlimit (0, getWidth()  - c->getWidth(),  c->getX() + delta.x);
        int y = juce::jlimit (0, getHeight() - c->getHeight(), c->getY() + delta.y);
        c->setTopLeftPosition (x, y);
        repaint();                  // redraw cables to follow the node live
    }
}

void CanvasView::persistNodePosition (const juce::String& uid)
{
    if (const auto* v = viewForUid (uid))
    {
        const auto norm = normForCentre (v->comp->getBounds().getCentre());
        controller.setNodePosition (uid, norm.x, norm.y);
    }
}

const CanvasView::NodeView* CanvasView::viewForUid (const juce::String& uid) const
{
    for (const auto& v : views)
        if (v.uid == uid)
            return &v;
    return nullptr;
}

juce::Point<float> CanvasView::outputPort (const juce::String& uid) const
{
    if (const auto* v = viewForUid (uid))
    {
        auto b = v->comp->getBounds().toFloat();
        return { b.getRight() + kPortGap, b.getCentreY() };
    }
    return {};
}

juce::Point<float> CanvasView::inputPort (const juce::String& uid) const
{
    if (const auto* v = viewForUid (uid))
    {
        auto b = v->comp->getBounds().toFloat();
        return { b.getX() - kPortGap, b.getCentreY() };
    }
    return {};
}

std::vector<CanvasView::Cable> CanvasView::buildCables() const
{
    std::vector<Cable> cables;
    for (const auto& conn : controller.getConnections())
    {
        // Collapse the stereo pair (ch0 + ch1) into a single drawn cable.
        const bool already = std::any_of (cables.begin(), cables.end(),
            [&] (const Cable& x) { return x.src == conn.srcUid && x.dst == conn.dstUid; });
        if (already)
            continue;
        cables.push_back ({ conn.srcUid, conn.dstUid, outputPort (conn.srcUid), inputPort (conn.dstUid) });
    }
    return cables;
}

juce::String CanvasView::outputPortAt (juce::Point<float> p) const
{
    for (const auto& v : views)
        if (v.hasOutput && outputPort (v.uid).getDistanceFrom (p) <= kOutPortHit)
            return v.uid;
    return {};
}

juce::String CanvasView::inputPortAt (juce::Point<float> p) const
{
    for (const auto& v : views)
        if (v.hasInput && inputPort (v.uid).getDistanceFrom (p) <= kInPortHit)
            return v.uid;
    return {};
}

juce::Path CanvasView::cablePath (juce::Point<float> a, juce::Point<float> b)
{
    const float dx = juce::jmax (40.0f, std::abs (b.x - a.x) * 0.5f);
    juce::Path p;
    p.startNewSubPath (a);
    p.cubicTo ({ a.x + dx, a.y }, { b.x - dx, b.y }, b);
    return p;
}

//== Mouse: wire drag + delete =================================================
void CanvasView::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.position;

    // Start a wire from an output port.
    if (const auto src = outputPortAt (p); src.isNotEmpty())
    {
        draggingWire = true;
        wireSrcUid   = src;
        wireEnd      = p;
        hoverDstUid  = {};
        hoverValid   = false;
        repaint();
        return;
    }

    // Otherwise: click a cable to delete it (both channels).
    for (const auto& cable : buildCables())
    {
        const auto path = cablePath (cable.a, cable.b);
        juce::Point<float> nearest;
        if (path.getNearestPoint (p, nearest); nearest.getDistanceFrom (p) <= kCableHit)
        {
            controller.disconnect (cable.src, 0, cable.dst, 0);
            controller.disconnect (cable.src, 1, cable.dst, 1);
            repaint();
            return;
        }
    }
}

void CanvasView::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingWire)
        return;

    wireEnd = e.position;
    hoverDstUid = inputPortAt (wireEnd);
    hoverValid  = hoverDstUid.isNotEmpty() && hoverDstUid != wireSrcUid
                  && controller.canConnect (wireSrcUid, 0, hoverDstUid, 0);
    repaint();
}

void CanvasView::mouseUp (const juce::MouseEvent&)
{
    if (! draggingWire)
        return;

    if (hoverValid)
    {
        controller.connect (wireSrcUid, 0, hoverDstUid, 0);   // stereo: both channels
        controller.connect (wireSrcUid, 1, hoverDstUid, 1);
    }

    draggingWire = false;
    wireSrcUid   = {};
    hoverDstUid  = {};
    hoverValid   = false;
    repaint();
}

//== Layout ====================================================================
void CanvasView::resized()
{
    for (const auto& v : views)
        placeNode (*v.comp, v.uid);

    addSlot.setBounds (16, 16, 70, 60);
}

//== Paint =====================================================================
void CanvasView::paint (juce::Graphics& g)
{
    juce::ColourGradient bg (LightHostLookAndFeel::appBgTop(), (float) getWidth() * 0.5f, -40.0f,
                             LightHostLookAndFeel::appBgBottom(), (float) getWidth() * 0.5f, (float) getHeight(), true);
    g.setGradientFill (bg);
    g.fillAll();

    // Cables (behind the node cards, which are child components).
    for (const auto& cable : buildCables())
    {
        const auto path = cablePath (cable.a, cable.b);
        g.setColour (kCableIdle);
        g.strokePath (path, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Traveling signal-flow dot.
        const float len = path.getLength();
        if (len > 1.0f)
        {
            const auto dot = path.getPointAlongPath (flowPhase * len);
            g.setColour (LightHostLookAndFeel::accent().withAlpha (0.9f));
            g.fillEllipse (dot.x - 3.0f, dot.y - 3.0f, 6.0f, 6.0f);
        }
    }

    // Ports.
    for (const auto& v : views)
    {
        if (v.hasOutput)
        {
            const auto pt = outputPort (v.uid);
            g.setColour (kPortIdle);
            g.fillEllipse (pt.x - 4.0f, pt.y - 4.0f, 8.0f, 8.0f);
        }
        if (v.hasInput)
        {
            const auto pt = inputPort (v.uid);
            const bool isHoverTarget = draggingWire && v.uid == hoverDstUid;
            g.setColour (isHoverTarget ? (hoverValid ? kValidGreen : kInvalidRed) : kPortIdle);
            if (isHoverTarget)
                g.fillEllipse (pt.x - 6.0f, pt.y - 6.0f, 12.0f, 12.0f);
            else
                g.fillEllipse (pt.x - 4.0f, pt.y - 4.0f, 8.0f, 8.0f);
        }
    }

    // Rubber-band cable while dragging a new connection.
    if (draggingWire)
    {
        const auto a = outputPort (wireSrcUid);
        const auto path = cablePath (a, wireEnd);
        g.setColour ((hoverDstUid.isNotEmpty() ? (hoverValid ? kValidGreen : kInvalidRed)
                                               : LightHostLookAndFeel::accent()).withAlpha (0.85f));
        g.strokePath (path, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

} // namespace lighthost::ui
