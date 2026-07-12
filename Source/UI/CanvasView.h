//
//  CanvasView.h — the node canvas (design spec §"1C — tactile"), now a free-form
//  editable node graph: IN / OUT / plugin cards are draggable to arbitrary 2D
//  positions, each node has a stereo input + output port, and cables are drawn
//  from the live graph's connections. Drag an output port to an input port to
//  connect (validated by GraphController::canConnect); click a cable to delete it.
//  Node set is enumerated from controller.document.getNodes(); positions round-trip
//  through GraphController::getNodeX/Y + setNodePosition. Meters + the signal-flow
//  dots are advanced from the MainComponent 60 fps timer via tick().
//

#pragma once

#include <JuceHeader.h>
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

// Floating "+ ADD" button — appends a plugin (kept from the serial design).
class AddSlot : public juce::Component
{
public:
    AddSlot() = default;

    std::function<void()> onClick;
    void mouseEnter (const juce::MouseEvent&) override { hot = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hot = false; repaint(); }
    void mouseDown  (const juce::MouseEvent&) override { if (onClick) onClick(); }
    void paint (juce::Graphics&) override;
private:
    bool hot = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AddSlot)
};

//==============================================================================
class CanvasView : public juce::Component
{
public:
    explicit CanvasView (GraphController&);

    // Supplied by the host (IconMenu).
    std::function<void (juce::Point<int> screenPos)> onAddPlugin;
    std::function<void (const juce::String& uid)>    onOpenEditor;

    void refresh();                 // rebuild node views from the document
    void tick();                    // 60 fps: meters + flow animation
    int  getContentWidth() const noexcept { return virtualW; }

    // Told by the host how much of the canvas is on-screen so the canvas can grow to
    // fill the viewport (no empty gap); node positions still map onto virtualW/virtualH.
    void setVisibleArea (int w, int h);

    // Default viewport height the window opens at (the virtual canvas is taller and
    // scrolls inside the viewport).
    static constexpr int canvasHeight = 420;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;

private:
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

    void rebuildViews();
    void applyCanvasSize();         // size the canvas to max(content, visible area)
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
    juce::String outputPortAt (juce::Point<float> p) const;   // "" if none
    juce::String inputPortAt  (juce::Point<float> p) const;
    static juce::Path cablePath (juce::Point<float> a, juce::Point<float> b);

    GraphController& controller;

    juce::OwnedArray<NodeCard> cards;
    EndpointNode inNode  { "IN"  };
    EndpointNode outNode { "OUT" };
    AddSlot addSlot;
    std::vector<NodeView> views;

    // Wire-drag state.
    bool draggingWire = false;
    juce::String wireSrcUid;
    juce::Point<float> wireEnd;
    juce::String hoverDstUid;
    bool hoverValid = false;

    float flowPhase = 0.0f;

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

    // On-screen viewport size (set by the host); the canvas grows to at least this so
    // there is no empty gap when the window is larger than the node content.
    int visibleW = 0;
    int visibleH = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CanvasView)
};

} // namespace lighthost::ui
