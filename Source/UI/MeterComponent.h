//
//  MeterComponent.h — a vertical stereo peak/RMS meter with a dB scale and
//  simple ballistics (fast attack, slow release, held peak).
//
//  Pure view: it is fed MeterTap::Levels snapshots on the message thread by the
//  owning component's timer and never reads the audio thread itself. It depends
//  only on MeterTap (a POD-ish value type), not on the graph/controller.
//

#pragma once

#include <JuceHeader.h>
#include "../Engine/MeterTap.h"
#include "LightHostLookAndFeel.h"

namespace lighthost::ui
{

class MeterComponent : public juce::Component
{
public:
    explicit MeterComponent (juce::String labelText)
        : label (std::move (labelText)) {}

    // Message thread: push the latest snapshot (pass all-zero Levels when the
    // corresponding tap is absent) and repaint. Applies release ballistics so
    // the bar falls smoothly instead of flickering per block.
    void setLevels (const MeterTap::Levels& lv)
    {
        for (int ch = 0; ch < MeterTap::numChannels; ++ch)
        {
            const float rms = lv.rms[ch];
            rmsDisplay[ch] = rms >= rmsDisplay[ch]
                                 ? rms
                                 : rmsDisplay[ch] * releaseCoef + rms * (1.0f - releaseCoef);

            const float pk = lv.peak[ch];
            if (pk >= peakHold[ch])
            {
                peakHold[ch]    = pk;
                peakHoldCtr[ch] = peakHoldFrames;
            }
            else if (peakHoldCtr[ch] > 0)
            {
                --peakHoldCtr[ch];
            }
            else
            {
                peakHold[ch] = peakHold[ch] * releaseCoef + pk * (1.0f - releaseCoef);
            }
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (4);

        auto labelArea = r.removeFromBottom (16);
        g.setColour (LightHostLookAndFeel::textDim());
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (label, labelArea, juce::Justification::centred);

        const int gap  = 4;
        const int barW = (r.getWidth() - gap) / 2;

        for (int ch = 0; ch < MeterTap::numChannels; ++ch)
        {
            auto bar = r.removeFromLeft (barW).toFloat();
            if (ch == 0)
                r.removeFromLeft (gap);
            drawBar (g, bar, rmsDisplay[ch], peakHold[ch]);
        }
    }

private:
    static constexpr float minDb = -60.0f;
    static constexpr float maxDb =   6.0f;

    static float normDb (float linear)
    {
        const float db = MeterTap::toDecibels (linear);
        return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
    }

    void drawBar (juce::Graphics& g, juce::Rectangle<float> bar, float rms, float peak)
    {
        g.setColour (LightHostLookAndFeel::panel());
        g.fillRoundedRectangle (bar, 2.0f);
        g.setColour (LightHostLookAndFeel::outline());
        g.drawRoundedRectangle (bar, 2.0f, 1.0f);

        auto fill = bar.reduced (1.5f);

        juce::ColourGradient grad (LightHostLookAndFeel::meterLow(),  fill.getX(), fill.getBottom(),
                                   LightHostLookAndFeel::meterHigh(), fill.getX(), fill.getY(), false);
        grad.addColour (0.70, LightHostLookAndFeel::meterLow());
        grad.addColour (0.85, LightHostLookAndFeel::meterMid());

        const float rmsN = normDb (rms);
        auto level = fill.removeFromBottom (fill.getHeight() * rmsN);
        g.setGradientFill (grad);
        g.fillRect (level);

        // Held-peak marker.
        const float pkN = normDb (peak);
        const float y   = bar.getBottom() - bar.getHeight() * pkN;
        g.setColour (peak > 1.0f ? LightHostLookAndFeel::meterHigh() : LightHostLookAndFeel::text());
        g.fillRect (bar.getX(), y - 1.0f, bar.getWidth(), 2.0f);
    }

    const juce::String label;

    // Ballistics state (per channel). Tuned for a ~60 fps feed.
    static constexpr float releaseCoef    = 0.82f;
    static constexpr int   peakHoldFrames = 45;   // ~0.75 s hold at 60 fps

    float rmsDisplay[MeterTap::numChannels] { 0.0f, 0.0f };
    float peakHold  [MeterTap::numChannels] { 0.0f, 0.0f };
    int   peakHoldCtr[MeterTap::numChannels] { 0, 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeterComponent)
};

} // namespace lighthost::ui
