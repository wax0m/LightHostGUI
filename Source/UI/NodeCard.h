//
//  NodeCard.h — one plugin in the chain, drawn as a tactile node card
//  (design spec §"Node card"). Header (status dot, name, format, bypass switch,
//  remove ×), divider, two knobs (THIS pass = host gain + pan), and the node's
//  stereo output meter (L/R wells).
//
//  Knob/bypass edits call the controller's live message-thread API (no graph
//  rebuild). Structural actions (remove, open editor) are delegated to the canvas
//  so the card never destroys itself mid-callback.
//

#pragma once

#include <JuceHeader.h>
#include "../Engine/GraphController.h"
#include "LightHostLookAndFeel.h"
#include "KnobStrip.h"
#include "SegmentedMeter.h"

namespace lighthost::ui
{

class NodeCard : public juce::Component
{
public:
    static constexpr int cardWidth  = 226;
    static constexpr int cardHeight = 300;

    NodeCard (GraphController& controller, GraphController::ChainItem item);

    const juce::String& getUid() const noexcept { return uid; }

    // Poll the node's output meter (called by the canvas timer).
    void updateMeters();

    // Canvas-supplied structural actions.
    std::function<void (const juce::String&)> onRemove;
    std::function<void (const juce::String&)> onOpenEditor;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void applyBypassLook();

    GraphController& controller;
    juce::String uid, name, format;
    bool bypassed = false;
    bool missing  = false;

    KnobStrip gainKnob { "Gain", 0.0, 2.0, 1.0 };
    KnobStrip panKnob  { "Pan", -1.0, 1.0, 0.0 };
    SegmentedMeter meterL, meterR;

    juce::Rectangle<int> bypassRect, removeRect, statusDotRect;
    bool removeHot = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NodeCard)
};

} // namespace lighthost::ui
