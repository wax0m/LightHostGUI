//
//  CanvasView.h — the node canvas (design spec §"1C — tactile"), a free-form editable
//  node graph: IN / OUT / plugin cards are draggable to arbitrary 2D positions, each
//  node has a stereo input + output port, and cables are drawn from the document's
//  connections. Positions round-trip through GraphController::getNodeX/Y +
//  setNodePosition. Meters + the signal-flow dots are advanced from the MainComponent
//  60 fps timer via tick().
//
//  Interaction:
//    - drag from an output port, drop on another node's input port OR anywhere on its
//      card -> connect (validated by GraphController::canConnectPair, green/red highlight);
//    - click a cable -> select it; delete with a double-click, the x badge, Delete/Backspace
//      or right-click;
//    - drag empty space (left or middle button) -> pan the viewport;
//    - Ctrl+wheel / pinch / Ctrl +,-,0 -> zoom requests (handled by MainComponent, which
//      owns the viewport and the CanvasZoomHolder).
//  The canvas always works in LOGICAL (unzoomed) coordinates; hit tolerances are divided
//  by the zoom so they stay constant in screen pixels.
//

#pragma once

#include <JuceHeader.h>
#include <optional>
#include "../Engine/GraphController.h"
#include "LightHostLookAndFeel.h"
#include "NodeCard.h"

namespace lighthost::ui
{

// Rounded chain-endpoint terminal (IN / OUT). A first-class draggable node: it
// carries the document uid of the audioIn/audioOut node and reports body drags the
// same way NodeCard does so the canvas can persist its position.
class EndpointNode : public juce::Component
{
public:
    explicit EndpointNode (juce::String labelText) : text (std::move (labelText)) {}

    juce::String uid;
    std::function<void (const juce::String&, juce::Point<int> delta)> onDragMove;
    std::function<void (const juce::String&)> onDragEnd;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;

private:
    juce::String text;
    bool nodeDragActive = false;
    juce::Point<int> lastParentPos;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EndpointNode)
};

//==============================================================================
class CanvasView : public juce::Component
{
public:
    explicit CanvasView (GraphController&);

    // Zoom range and step (MainComponent applies them; the canvas only requests).
    static constexpr float kMinZoom  = 0.3f;
    static constexpr float kMaxZoom  = 2.0f;
    static constexpr float kZoomStep = 1.2f;

    // Supplied by the host (MainComponent / IconMenu).
    std::function<void (const juce::String& uid)> onOpenEditor;
    // factor > 1 zooms in. screenAnchor = the point to keep fixed (nullopt = view centre).
    std::function<void (float factor, std::optional<juce::Point<int>> screenAnchor)> onZoomBy;
    std::function<void()> onZoomReset;
    std::function<void()> onZoomFit;

    void refresh();                 // rebuild node views from the document
    void tick();                    // 60 fps: meters + flow animation
    int  getContentWidth() const noexcept { return virtualW; }

    // Told by the host the viewport's pixel size and the current zoom. Node positions map
    // onto a layout area of max(content, viewport pixels) that does NOT depend on the zoom
    // (zooming never moves nodes); the canvas itself grows to fill the viewport at any zoom.
    void setViewportSize (int widthPx, int heightPx, float zoom);

    // Logical bounds of one node / of all nodes (for reveal and zoom-to-fit). Empty if none.
    juce::Rectangle<int> getNodeBounds (const juce::String& uid) const;
    juce::Rectangle<int> getContentBounds() const;

    // Default viewport height the window opens at (the virtual canvas is taller and
    // scrolls inside the viewport).
    static constexpr int canvasHeight = 420;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void markRetryFailed (const juce::String& uid);   // tell the missing card its retry failed

    // One placed node (IN, OUT, or a plugin card) plus which ports it exposes.
    struct NodeView
    {
        juce::String uid;
        juce::Component* comp = nullptr;
        bool hasInput  = true;
        bool hasOutput = true;
    };

    // A drawn cable between two nodes (both stereo channels collapsed to one line).
    struct Cable { juce::String src, dst; juce::Point<float> a, b; };
    struct CableKey
    {
        juce::String src, dst;
        bool operator== (const CableKey& o) const { return src == o.src && dst == o.dst; }
    };

    void rebuildViews();
    void applyCanvasSize();         // layout area + canvas size from content, viewport and zoom
    void autoLayoutIfStacked();
    juce::Point<int>   centreForNorm (float nx, float ny) const;
    juce::Point<float> normForCentre (juce::Point<int> centre) const;
    void placeNode (juce::Component& c, const juce::String& uid);
    void moveNodeBy (const juce::String& uid, juce::Point<int> delta);
    void persistNodePosition (const juce::String& uid);

    const NodeView* viewForUid (const juce::String& uid) const;
    juce::Point<float> outputPort (const juce::String& uid) const;
    juce::Point<float> inputPort  (const juce::String& uid) const;
    std::vector<Cable> buildCables() const;
    juce::String outputPortAt  (juce::Point<float> p) const;   // "" if none
    juce::String inputTargetAt (juce::Point<float> p) const;   // input port OR anywhere on a node
    std::optional<Cable> cableAt (juce::Point<float> p) const;
    static juce::Path cablePath (juce::Point<float> a, juce::Point<float> b);

    // Cable selection (click selects, x badge / Delete / right-click removes).
    std::optional<Cable> selectedCable() const;          // re-resolved against the current document
    std::optional<juce::Point<float>> deleteBadgeCentre() const;
    void deleteSelectedCable();
    void showCableMenu (const Cable&);

    juce::Viewport* findViewport() const { return findParentComponentOfClass<juce::Viewport>(); }

    GraphController& controller;

    juce::OwnedArray<NodeCard> cards;
    EndpointNode inNode  { "IN"  };
    EndpointNode outNode { "OUT" };
    std::vector<NodeView> views;

    // Wire-drag state.
    bool draggingWire = false;
    juce::String wireSrcUid;
    juce::Point<float> wireEnd;
    juce::String hoverDstUid;
    bool hoverValid = false;

    // Pan state: armed on a press in empty space (or on a cable), active once dragged.
    bool panArmed = false;
    bool panning  = false;
    juce::Point<int> panStartScreen;
    juce::Point<int> panStartView;
    std::optional<CableKey> pressedCable;   // press on a cable: select it on release unless it became a pan
    std::optional<CableKey> selection;

    float flowPhase = 0.0f;
    float zoom = 1.0f;                       // for zoom-invariant hit tolerances

    // Virtual (scrolled) canvas size and node-centre margins.
    int virtualW = 1200;
    int virtualH = 700;
    static constexpr int marginX = 140;   // ~half card width + gutter
    static constexpr int marginY = 170;   // ~half card height + gutter

    // Layout tuning. Plugin cards auto-lay-out across the normalised [bandLo..bandHi]
    // band (endpoints sit outside it: IN at 0.05, OUT at 0.95); refresh() scales the
    // virtual width so adjacent plugin centres stay >= nodeSlot apart (no overlap).
    static constexpr int   nodeSlot      = 270;    // card width (226) + gutter
    static constexpr float layoutBandLo  = 0.2f;
    static constexpr float layoutBandHi  = 0.8f;
    static constexpr float layoutBandSpan = layoutBandHi - layoutBandLo;

    // Viewport size in pixels (set by the host) and the zoom-independent layout area that
    // normalised node positions map onto. The canvas may be larger (zoomed out: filler).
    int viewportW = 0;
    int viewportH = 0;
    int layoutW = 1200;
    int layoutH = 700;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CanvasView)
};

} // namespace lighthost::ui
