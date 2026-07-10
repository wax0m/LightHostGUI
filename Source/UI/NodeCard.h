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

    // Canvas-supplied node move: dragging the card BODY (not a knob or a header
    // control) reports a pixel delta in parent (canvas) coords, then a drag-end so
    // the canvas can persist the normalized position.
    std::function<void (const juce::String&, juce::Point<int> delta)> onDragMove;
    std::function<void (const juce::String&)> onDragEnd;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void applyBypassLook();
    void setupGainPanKnobs();                                    // host channel strip
    void setupPluginParamKnobs (const std::vector<params::ParamInfo>&);  // hosted-plugin params
    void refreshParamReadouts();                                 // poll .value/.text on the timer

    GraphController& controller;
    juce::String uid, name, format;
    bool bypassed = false;
    bool missing  = false;

    // The two knob columns. They drive either the host gain/pan strip or the first
    // two curated plugin parameters — paramIndices decides (empty => host mode).
    KnobStrip knobA { "Gain", 0.0, 2.0, 1.0 };
    KnobStrip knobB { "Pan", -1.0, 1.0, 0.0 };
    std::vector<int> paramIndices;   // knob slot -> plugin param index
    int paramPollCounter = 0;
    SegmentedMeter meterL, meterR;

    // Per-node MIDI enable (M7). Shown only for plugins that acceptsMidi(); a
    // compact "MIDI" pill in the header toggling the document's receivesMidi flag.
    bool showMidi = false;
    bool midiOn   = false;

    juce::Rectangle<int> bypassRect, removeRect, statusDotRect, midiRect;
    bool removeHot = false;

    // Body-drag (node move) state.
    bool nodeDragActive = false;
    juce::Point<int> lastParentPos;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NodeCard)
};

} // namespace lighthost::ui
