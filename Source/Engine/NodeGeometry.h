//
//  NodeGeometry.h — the one source for canvas sizes shared by the engine and the UI.
//
//  Node positions in the GRAPH document (version 2) are the node's CENTRE in logical canvas
//  pixels at 100 % zoom, origin = canvas top-left. The engine needs the card sizes for
//  migration and for placing new cards, so they live here instead of in the UI.
//

#pragma once

#include <JuceHeader.h>

namespace lighthost::geometry
{
    constexpr int   cardWidth      = 226;
    constexpr int   cardHeight     = 300;
    constexpr int   endpointWidth  = 54;
    constexpr int   endpointHeight = 150;
    constexpr int   nodeSlot       = 270;   // centre to centre of two cards in a row
    constexpr int   nodeGap        = 40;    // minimum clear space between two nodes

    constexpr float kMaxCoord      = 20000.0f;

    // A missing or non-finite position.
    constexpr float fallbackX      = 400.0f;
    constexpr float fallbackY      = 260.0f;

    // Defaults of a fresh document.
    constexpr float defaultInX     = 100.0f;
    constexpr float defaultOutX    = 640.0f;
    constexpr float defaultY       = 260.0f;

    inline juce::Rectangle<float> nodeRect (bool endpoint, juce::Point<float> centre)
    {
        return juce::Rectangle<float> ((float) (endpoint ? endpointWidth  : cardWidth),
                                       (float) (endpoint ? endpointHeight : cardHeight)).withCentre (centre);
    }
}
