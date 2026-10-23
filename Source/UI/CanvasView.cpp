#include "CanvasView.h"

namespace lighthost::ui
{

// Palette for the wiring layer (kept local; reuses the app accent for flow).
static const juce::Colour kCableIdle   { 0xff6a6058 };
static const juce::Colour kPortIdle    { 0xff8a8078 };
static const juce::Colour kValidGreen  { 0xff5ec27a };
static const juce::Colour kInvalidRed  { 0xffd25b5b };

// Hit tolerances in SCREEN pixels; divided by the zoom before use (logical coords).
static constexpr float kOutPortHit     = 12.0f;   // start a wire from an output port
static constexpr float kInPortHit      = 18.0f;   // drop tolerance onto an input port
static constexpr float kBodyDropMargin = 10.0f;   // drop onto a node: its bounds grown by this
static constexpr float kCableHit       = 6.0f;    // click tolerance to select a cable
static constexpr float kPanThreshold   = 3.0f;    // drag distance before a press becomes a pan
// In LOGICAL pixels (scales with the canvas, like the ports).
static constexpr float kPortGap        = 8.0f;    // port dot offset outside the card edge
static constexpr float kBadgeRadius    = 9.0f;    // delete badge on the selected cable

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
    if (forwardMiddleToParent (*this, e, &juce::Component::mouseDown))
        return;
    if (auto* parent = getParentComponent())
        parent->grabKeyboardFocus();
    nodeDragActive = true;
    lastParentPos  = e.getEventRelativeTo (getParentComponent()).getPosition();
}

void EndpointNode::mouseDrag (const juce::MouseEvent& e)
{
    if (forwardMiddleToParent (*this, e, &juce::Component::mouseDrag))
        return;
    if (! nodeDragActive)
        return;
    const auto p = e.getEventRelativeTo (getParentComponent()).getPosition();
    const auto delta = p - lastParentPos;
    lastParentPos = p;
    if (onDragMove) onDragMove (uid, delta);
}

void EndpointNode::mouseUp (const juce::MouseEvent& e)
{
    if (forwardMiddleToParent (*this, e, &juce::Component::mouseUp))
        return;
    if (nodeDragActive)
    {
        nodeDragActive = false;
        if (onDragEnd) onDragEnd (uid);
    }
}

//== CanvasView ================================================================
CanvasView::CanvasView (GraphController& c) : controller (c)
{
    setWantsKeyboardFocus (true);   // Delete / Esc / Ctrl +,-,0

    inNode.setSize (lighthost::geometry::endpointWidth, lighthost::geometry::endpointHeight);
    outNode.setSize (lighthost::geometry::endpointWidth, lighthost::geometry::endpointHeight);
    addAndMakeVisible (inNode);
    addAndMakeVisible (outNode);

    auto wireEndpoint = [this] (EndpointNode& n)
    {
        n.onDragMove = [this] (const juce::String& uid, juce::Point<int> d) { moveNodeBy (uid, d); };
        n.onDragEnd  = [this] (const juce::String& uid) { persistNodePosition (uid); };
    };
    wireEndpoint (inNode);
    wireEndpoint (outNode);

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
            item.missing  = controller.isNodeUnavailable (uid);

            auto* card = new NodeCard (controller, item);
            card->onOpenEditor = [this] (const juce::String& u) { if (onOpenEditor) onOpenEditor (u); };
            card->onRemove = [this] (const juce::String& u)
            {
                // Defer so the card isn't destroyed inside its own mouse callback.
                // SafePointer: delivery can outlive this CanvasView (shutdown /
                // nested message loops), and the callback must then be a no-op.
                auto* self = this;   // MSVC rejects `this` inside the nested init-capture
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<CanvasView> (self), u]
                {
                    if (safe == nullptr)
                        return;
                    safe->controller.removeNode (u);   // heals around the node; branches survive
                    safe->refresh (true);              // keep the view where it is: no jump after a delete
                });
            };
            card->onRetry = [this] (const juce::String& u)
            {
                // Deferred like onRemove: a successful retry refresh()es, which would
                // otherwise destroy the card inside its own mouse callback.
                auto* self = this;   // MSVC rejects `this` inside the nested init-capture
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<CanvasView> (self), u]
                {
                    if (safe == nullptr)
                        return;
                    if (safe->controller.retryMissingPlugin (u))
                        safe->refresh();             // the card comes back as the live plugin
                    else
                        safe->markRetryFailed (u);   // stand-in kept, nothing published
                });
            };
            card->onDragMove = [this] (const juce::String& u, juce::Point<int> d) { moveNodeBy (u, d); };
            card->onDragEnd  = [this] (const juce::String& u) { persistNodePosition (u); };
            addAndMakeVisible (card);
            cards.add (card);
            views.push_back ({ uid, card, true, true });
        }
    }
}

void CanvasView::markRetryFailed (const juce::String& uid)
{
    for (auto* card : cards)
        if (card->getUid() == uid)
            card->markRetryFailed();
}

void CanvasView::refresh (bool keepVisible)
{
    rebuildViews();

    // A selection whose cable no longer exists (deleted, preset switch) is dropped.
    if (selection.has_value() && ! selectedCable().has_value())
        selection.reset();

    applyCanvasSize (keepVisible);
    repaint();
}

void CanvasView::setViewportSize (int widthPx, int heightPx, float newZoom)
{
    if (widthPx == viewportW && heightPx == viewportH && newZoom == zoom)
        return;
    viewportW = widthPx;
    viewportH = heightPx;
    zoom = newZoom;
    applyCanvasSize();
}

void CanvasView::applyCanvasSize (bool keepVisible)
{
    // Content = the union of the stored node rects (read from the document, so it is right even
    // before the views are placed). The canvas holds the content plus room for one more card, and
    // always fills the viewport at any zoom. floor, not ceil: the scaled size must never exceed the
    // viewport by a rounding pixel, or a scrollbar appears for nothing.
    using namespace lighthost::geometry;
    juce::Rectangle<float> content;
    for (const auto& v : views)
    {
        const auto r = nodeRect (v.comp == &inNode || v.comp == &outNode,
                                 { controller.getNodeX (v.uid), controller.getNodeY (v.uid) });
        content = content.isEmpty() ? r : content.getUnion (r);
    }

    int fillW = (int) std::floor ((float) viewportW / zoom);
    int fillH = (int) std::floor ((float) viewportH / zoom);

    // After a drop or delete the canvas must not shrink below what is on screen: the viewport would clamp
    // its position and the whole picture would jump. It shrinks on the next full recompute after scrolling back.
    if (keepVisible)
        if (const auto* vp = findViewport())
        {
            fillW = juce::jmax (fillW, (int) std::floor ((float) (vp->getViewPositionX() + vp->getMaximumVisibleWidth())  / zoom));
            fillH = juce::jmax (fillH, (int) std::floor ((float) (vp->getViewPositionY() + vp->getMaximumVisibleHeight()) / zoom));
        }

    setSize (juce::jmax ((int) std::ceil (content.getRight()) + nodeSlot, fillW),
             juce::jmax ((int) std::ceil (content.getBottom()) + cardHeight / 2, fillH));
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

juce::Rectangle<int> CanvasView::getNodeBounds (const juce::String& uid) const
{
    if (const auto* v = viewForUid (uid))
        return v->comp->getBounds();
    return {};
}

juce::Rectangle<int> CanvasView::getContentBounds() const
{
    juce::Rectangle<int> r;
    for (const auto& v : views)
        r = r.isEmpty() ? v.comp->getBounds() : r.getUnion (v.comp->getBounds());
    return r;
}

//== Geometry ==================================================================
// Stored positions are the node centre in logical px, so place <-> persist are exact inverses.
void CanvasView::placeNode (juce::Component& c, const juce::String& uid)
{
    const juce::Point<int> centre { juce::roundToInt (controller.getNodeX (uid)), juce::roundToInt (controller.getNodeY (uid)) };
    c.setBounds (juce::Rectangle<int> (c.getWidth(), c.getHeight()).withCentre (centre));
}

void CanvasView::moveNodeBy (const juce::String& uid, juce::Point<int> delta)
{
    if (const auto* v = viewForUid (uid))
    {
        auto* c = v->comp;
        // Top-left stays on the canvas and the centre within the document's coordinate range.
        const int maxCoord = (int) lighthost::geometry::kMaxCoord;
        const int x = juce::jlimit (0, maxCoord - c->getWidth()  / 2, c->getX() + delta.x);
        const int y = juce::jlimit (0, maxCoord - c->getHeight() / 2, c->getY() + delta.y);
        c->setTopLeftPosition (x, y);

        // Past the right / bottom edge the canvas grows live (it only shrinks again on drop). The
        // document still holds the old position until then, so resized() must not re-place nodes.
        const int needW = c->getRight()  + lighthost::geometry::nodeSlot;
        const int needH = c->getBottom() + lighthost::geometry::cardHeight / 2;
        if (needW > getWidth() || needH > getHeight())
        {
            const juce::ScopedValueSetter<bool> hold (suppressPlacement, true);
            setSize (juce::jmax (getWidth(), needW), juce::jmax (getHeight(), needH));
        }
        repaint();                  // redraw cables to follow the node live
    }
}

void CanvasView::persistNodePosition (const juce::String& uid)
{
    if (const auto* v = viewForUid (uid))
    {
        const auto centre = v->comp->getBounds().getCentre();
        controller.setNodePosition (uid, (float) centre.x, (float) centre.y);
        applyCanvasSize (true);     // full recompute: the canvas may shrink back after a drop, but never below the view
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
        if (v.hasOutput && outputPort (v.uid).getDistanceFrom (p) <= kOutPortHit / zoom)
            return v.uid;
    return {};
}

juce::String CanvasView::inputTargetAt (juce::Point<float> p) const
{
    // 1. The input port itself (the original, precise target).
    for (const auto& v : views)
        if (v.hasInput && inputPort (v.uid).getDistanceFrom (p) <= kInPortHit / zoom)
            return v.uid;

    // 2. Anywhere on a node, plus a small margin: dropping on the card is the natural
    //    gesture. Topmost first (later views are added later, so they paint above).
    for (auto it = views.rbegin(); it != views.rend(); ++it)
        if (it->hasInput && it->comp->getBounds().toFloat().expanded (kBodyDropMargin / zoom).contains (p))
            return it->uid;

    return {};
}

std::optional<CanvasView::Cable> CanvasView::cableAt (juce::Point<float> p) const
{
    for (const auto& cable : buildCables())
    {
        const auto path = cablePath (cable.a, cable.b);
        juce::Point<float> nearest;
        path.getNearestPoint (p, nearest);
        if (nearest.getDistanceFrom (p) <= kCableHit / zoom)
            return cable;
    }
    return std::nullopt;
}

juce::Path CanvasView::cablePath (juce::Point<float> a, juce::Point<float> b)
{
    const float dx = juce::jmax (40.0f, std::abs (b.x - a.x) * 0.5f);
    juce::Path p;
    p.startNewSubPath (a);
    p.cubicTo ({ a.x + dx, a.y }, { b.x - dx, b.y }, b);
    return p;
}

//== Cable selection ===========================================================
std::optional<CanvasView::Cable> CanvasView::selectedCable() const
{
    if (! selection.has_value())
        return std::nullopt;
    for (const auto& cable : buildCables())
        if (cable.src == selection->src && cable.dst == selection->dst)
            return cable;
    return std::nullopt;
}

std::optional<juce::Point<float>> CanvasView::deleteBadgeCentre() const
{
    if (const auto cable = selectedCable())
    {
        const auto path = cablePath (cable->a, cable->b);
        return path.getPointAlongPath (path.getLength() * 0.5f);
    }
    return std::nullopt;
}

void CanvasView::deleteSelectedCable()
{
    if (const auto cable = selectedCable())
        controller.disconnectPair (cable->src, cable->dst);   // every edge of the pair, one rewire
    selection.reset();
    repaint();
}

void CanvasView::showCableMenu (const Cable& cable)
{
    selection = CableKey { cable.src, cable.dst };
    repaint();

    juce::PopupMenu m;
    m.addItem (1, "Delete connection");
    auto* self = this;   // MSVC rejects `this` inside the nested init-capture
    m.showMenuAsync (juce::PopupMenu::Options(),
        [safe = juce::Component::SafePointer<CanvasView> (self), key = CableKey { cable.src, cable.dst }] (int r)
        {
            if (safe == nullptr || r != 1)
                return;
            safe->selection = key;
            safe->deleteSelectedCable();
        });
}

//== Mouse: wire drag, cable select, pan =======================================
void CanvasView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    const auto p = e.position;
    panArmed = panning = false;
    pressedCable.reset();

    // Right-click on a cable: context menu.
    if (e.mods.isPopupMenu())
    {
        if (const auto cable = cableAt (p))
            showCableMenu (*cable);
        return;
    }

    // Middle button: pan from anywhere on the canvas background.
    const bool middle = e.mods.isMiddleButtonDown();

    // The x badge on the selected cable deletes it.
    if (! middle)
        if (const auto badge = deleteBadgeCentre(); badge.has_value() && badge->getDistanceFrom (p) <= kBadgeRadius + 2.0f)
        {
            deleteSelectedCable();
            return;
        }

    // Start a wire from an output port.
    if (! middle)
        if (const auto src = outputPortAt (p); src.isNotEmpty())
        {
            draggingWire = true;
            wireSrcUid   = src;
            wireEnd      = p;
            hoverDstUid  = {};
            hoverValid   = false;
            beginDragAutoRepeat (30);   // keep auto-scrolling while the mouse rests at an edge
            repaint();
            return;
        }

    // A press on a cable selects it on release -- unless the press turns into a pan.
    if (! middle)
        if (const auto cable = cableAt (p))
            pressedCable = CableKey { cable->src, cable->dst };

    // Empty space (or a cable): arm a pan; it starts once the mouse actually moves.
    if (auto* vp = findViewport())
    {
        panArmed       = true;
        panStartScreen = e.getScreenPosition();
        panStartView   = vp->getViewPosition();
    }
}

void CanvasView::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingWire)
    {
        wireEnd = e.position;

        // Dragging a wire toward the viewport edge scrolls the canvas.
        if (auto* vp = findViewport())
        {
            const auto inViewport = vp->getLocalPoint (nullptr, e.getScreenPosition());
            vp->autoScroll (inViewport.x, inViewport.y, 40, 14);
        }

        // Validity only depends on the hovered target, not the cursor position —
        // re-run the (cycle-DFS) validation only when the target actually changes.
        if (const juce::String dst = inputTargetAt (wireEnd); dst != hoverDstUid)
        {
            hoverDstUid = dst;
            hoverValid  = dst.isNotEmpty() && dst != wireSrcUid
                          && controller.canConnectPair (wireSrcUid, dst);
        }
        repaint();
        return;
    }

    if (! panArmed)
        return;

    if (! panning && e.getDistanceFromDragStart() > kPanThreshold)
    {
        panning = true;
        pressedCable.reset();    // it was a pan, not a click on the cable
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    if (panning)
        if (auto* vp = findViewport())
            vp->setViewPosition (panStartView - (e.getScreenPosition() - panStartScreen));
}

void CanvasView::mouseUp (const juce::MouseEvent&)
{
    if (draggingWire)
    {
        if (hoverValid)
            controller.connectPair (wireSrcUid, hoverDstUid);   // atomic stereo pair, one rewire

        draggingWire = false;
        wireSrcUid   = {};
        hoverDstUid  = {};
        hoverValid   = false;
        repaint();
        return;
    }

    if (panArmed && ! panning)
    {
        // A click without movement: select the pressed cable, or clear the selection.
        if (pressedCable.has_value())
            selection = pressedCable;
        else
            selection.reset();
        repaint();
    }

    if (panning)
        setMouseCursor (juce::MouseCursor::NormalCursor);

    panArmed = panning = false;
    pressedCable.reset();
}

// Double-click on a cable deletes it. JUCE sends this after the SECOND mouseUp, so by now
// the first click has selected the cable (and the second may already have hit the x badge,
// in which case there is no cable left here and nothing happens).
void CanvasView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || e.mods.isMiddleButtonDown())
        return;

    if (const auto cable = cableAt (e.position))
    {
        controller.disconnectPair (cable->src, cable->dst);   // every edge of the pair, one rewire
        selection.reset();
        pressedCable.reset();
        repaint();
    }
}

void CanvasView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if ((e.mods.isCommandDown() || e.mods.isCtrlDown()) && onZoomBy)
    {
        const float delta = wheel.deltaY != 0.0f ? wheel.deltaY : wheel.deltaX;
        if (delta == 0.0f)
            return;
        // A mouse-wheel notch is one zoom step; a smooth (trackpad) stream scales with it.
        const float factor = wheel.isSmooth ? std::pow (kZoomStep, delta * 4.0f)
                                            : (delta > 0.0f ? kZoomStep : 1.0f / kZoomStep);
        onZoomBy (factor, e.getScreenPosition());
        return;
    }

    juce::Component::mouseWheelMove (e, wheel);   // plain wheel: let the viewport scroll
}

void CanvasView::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    if (onZoomBy)
        onZoomBy (scaleFactor, e.getScreenPosition());
}

bool CanvasView::keyPressed (const juce::KeyPress& key)
{
    if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && selection.has_value())
    {
        deleteSelectedCable();
        return true;
    }

    if (key == juce::KeyPress::escapeKey)
    {
        draggingWire = false;
        hoverDstUid  = {};
        hoverValid   = false;
        selection.reset();
        repaint();
        return true;
    }

    if (key.getModifiers().isCommandDown())
    {
        // Match the key code AND the typed character: on a Czech layout "+" and "0" are
        // reached via different physical keys than on a US one.
        const int code = key.getKeyCode();
        const juce::juce_wchar ch = key.getTextCharacter();
        const bool zoomIn    = code == '=' || code == '+' || code == juce::KeyPress::numberPadAdd      || ch == '+' || ch == '=';
        const bool zoomOut   = code == '-' || code == juce::KeyPress::numberPadSubtract                || ch == '-';
        const bool zoomReset = code == '0' || code == juce::KeyPress::numberPad0                       || ch == '0';
        if (zoomIn && onZoomBy)        { onZoomBy (kZoomStep, std::nullopt);        return true; }
        if (zoomOut && onZoomBy)       { onZoomBy (1.0f / kZoomStep, std::nullopt); return true; }
        if (zoomReset && onZoomReset)  { onZoomReset();                              return true; }
    }
    return false;
}

//== Layout ====================================================================
void CanvasView::resized()
{
    if (suppressPlacement)
        return;
    for (const auto& v : views)
        placeNode (*v.comp, v.uid);
}

//== Paint =====================================================================
void CanvasView::paint (juce::Graphics& g)
{
    juce::ColourGradient bg (LightHostLookAndFeel::appBgTop(), (float) getWidth() * 0.5f, -40.0f,
                             LightHostLookAndFeel::appBgBottom(), (float) getWidth() * 0.5f, (float) getHeight(), true);
    g.setGradientFill (bg);
    g.fillAll();

    const auto cables = buildCables();
    std::optional<Cable> sel;
    if (selection.has_value())
        for (const auto& c : cables)
            if (c.src == selection->src && c.dst == selection->dst)
                sel = c;

    // Cables (behind the node cards, which are child components).
    for (const auto& cable : cables)
    {
        const bool isSelected = sel.has_value() && sel->src == cable.src && sel->dst == cable.dst;
        const auto path = cablePath (cable.a, cable.b);
        g.setColour (isSelected ? LightHostLookAndFeel::accent() : kCableIdle);
        g.strokePath (path, juce::PathStrokeType (isSelected ? 3.4f : 2.4f,
                                                  juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Traveling signal-flow dot.
        const float len = path.getLength();
        if (len > 1.0f)
        {
            const auto dot = path.getPointAlongPath (flowPhase * len);
            g.setColour (LightHostLookAndFeel::accent().withAlpha (0.9f));
            g.fillEllipse (dot.x - 3.0f, dot.y - 3.0f, 6.0f, 6.0f);
        }
    }

    // Wire drop target: outline the whole node (the drop zone), not just its port.
    if (draggingWire && hoverDstUid.isNotEmpty())
        if (const auto* v = viewForUid (hoverDstUid))
        {
            g.setColour ((hoverValid ? kValidGreen : kInvalidRed).withAlpha (0.85f));
            g.drawRoundedRectangle (v->comp->getBounds().toFloat().expanded (5.0f), 13.0f, 2.0f);
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

    // Delete badge on the selected cable.
    if (sel.has_value())
    {
        const auto selPath = cablePath (sel->a, sel->b);
        const auto badge = selPath.getPointAlongPath (selPath.getLength() * 0.5f);
        const auto r = juce::Rectangle<float> (kBadgeRadius * 2.0f, kBadgeRadius * 2.0f).withCentre (badge);
        g.setColour (LightHostLookAndFeel::nodeTop());
        g.fillEllipse (r);
        g.setColour (LightHostLookAndFeel::accent());
        g.drawEllipse (r, 1.5f);
        g.setFont (LightHostLookAndFeel::display (14.0f));
        g.drawText (juce::String::fromUTF8 ("\xc3\x97"), r, juce::Justification::centred);   // ×
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
