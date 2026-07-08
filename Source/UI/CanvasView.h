//
//  CanvasView.h — the node canvas (design spec §"1C — tactile"): a horizontal
//  serial chain of IN → connectors → node cards → OUT → + ADD. Rebuilt from
//  GraphController::getChain(); meters + the signal-flow dots are advanced from
//  the MainComponent 60 fps timer via tick().
//

#pragma once

#include <JuceHeader.h>
#include "../Engine/GraphController.h"
#include "LightHostLookAndFeel.h"
#include "NodeCard.h"

namespace lighthost::ui
{

// Rounded chain-endpoint terminal (IN / OUT).
class EndpointNode : public juce::Component
{
public:
    explicit EndpointNode (juce::String labelText) : text (std::move (labelText)) {}
    void paint (juce::Graphics&) override;
private:
    juce::String text;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EndpointNode)
};

// Tactile connector: bar + accent arrowhead + a traveling signal-flow dot.
class Connector : public juce::Component
{
public:
    Connector() { setInterceptsMouseClicks (false, false); }
    void setPhase (float p) { phase = p; repaint(); }
    void paint (juce::Graphics&) override;
private:
    float phase = 0.0f;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Connector)
};

// Dashed "+ ADD" slot after OUT — appends a plugin.
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

    void refresh();                 // rebuild cards from the chain
    void tick();                    // 60 fps: meters + flow animation
    int  getContentWidth() const noexcept { return contentWidth; }

    static constexpr int canvasHeight = 360;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    GraphController& controller;

    juce::OwnedArray<NodeCard>  cards;
    juce::OwnedArray<Connector> connectors;
    EndpointNode inNode  { "IN"  };
    EndpointNode outNode { "OUT" };
    AddSlot addSlot;

    float flowPhase   = 0.0f;
    int   contentWidth = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CanvasView)
};

} // namespace lighthost::ui
