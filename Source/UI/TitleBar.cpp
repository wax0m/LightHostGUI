#include "TitleBar.h"

namespace lighthost::ui
{

TitleBar::TitleBar() { setInterceptsMouseClicks (true, true); }

juce::StringArray TitleBar::presetNames() const
{
    juce::StringArray names;
    if (getPresetCount)
        for (int i = 0, n = getPresetCount(); i < n; ++i)
            names.add (getPresetName ? getPresetName (i) : juce::String());
    return names;
}

void TitleBar::refreshPresets()
{
    resized();
    repaint();
}

void TitleBar::resized()
{
    tabRects.clear();
    auto r = getLocalBounds();
    r.removeFromLeft (16 + 14 + 8 + 90 + 14);   // logo + wordmark + divider

    auto font = LightHostLookAndFeel::display (13.0f, false);
    for (const auto& t : presetNames())
    {
        const int w = juce::GlyphArrangement::getStringWidthInt (font, t) + 30;
        tabRects.add (r.removeFromLeft (w));
    }

    plusRect = r.removeFromLeft (26);   // "+" add-preset tab

    settingsRect = getLocalBounds().removeFromRight (14).withSizeKeepingCentre (30, 26).translated (-8, 0);
}

void TitleBar::paint (juce::Graphics& g)
{
    auto b = getLocalBounds();

    juce::ColourGradient grad (LightHostLookAndFeel::titleTop(), 0.0f, 0.0f,
                               LightHostLookAndFeel::titleBottom(), 0.0f, (float) getHeight(), false);
    g.setGradientFill (grad);
    g.fillRect (b);
    g.setColour (LightHostLookAndFeel::titleBorder());
    g.fillRect (b.removeFromBottom (1));

    // Diamond logo.
    auto logo = juce::Rectangle<float> (13, 13).withCentre ({ 26.0f, getHeight() * 0.5f });
    juce::Path diamond;
    diamond.addRectangle (logo);
    g.setColour (LightHostLookAndFeel::accent());
    g.fillPath (diamond, juce::AffineTransform::rotation (juce::MathConstants<float>::pi * 0.25f, logo.getCentreX(), logo.getCentreY()));

    // Wordmark.
    g.setColour (LightHostLookAndFeel::textPrimary());
    g.setFont (LightHostLookAndFeel::display (14.5f, true));
    g.drawText ("LightHost", 40, 0, 100, getHeight(), juce::Justification::centredLeft);

    // Divider after wordmark.
    g.setColour (juce::Colour (0xff3a352f));
    g.fillRect (128, getHeight() / 2 - 9, 1, 18);

    // Preset tabs (read live from the store).
    const auto names  = presetNames();
    const int  active = getActivePreset ? getActivePreset() : -1;
    g.setFont (LightHostLookAndFeel::display (13.0f, false));
    for (int i = 0; i < tabRects.size() && i < names.size(); ++i)
    {
        const bool isActive = (i == active);
        g.setColour (isActive ? juce::Colour (0xfff0ebe3) : LightHostLookAndFeel::textSecond());
        g.drawText (names[i], tabRects[i], juce::Justification::centred);
        if (isActive)
        {
            g.setColour (LightHostLookAndFeel::accent());
            g.fillRect (tabRects[i].getX() + 6, getHeight() - 2, tabRects[i].getWidth() - 12, 2);
        }
    }

    // "+" add-preset tab.
    g.setColour (LightHostLookAndFeel::textTert());
    g.setFont (LightHostLookAndFeel::display (17.0f, false));
    g.drawText ("+", plusRect, juce::Justification::centred);

    // Right cluster: CPU meter + sample rate.
    auto rc = settingsRect.getX() - 12;
    g.setColour (LightHostLookAndFeel::textTert2());
    g.setFont (LightHostLookAndFeel::mono (10.0f));
    g.drawText ("48.0 kHz", rc - 66, 0, 62, getHeight(), juce::Justification::centredRight);

    const int meterX = rc - 66 - 8 - 46;
    juce::Rectangle<float> cpuTrack ((float) meterX, getHeight() * 0.5f - 2.5f, 46.0f, 5.0f);
    g.setColour (juce::Colour (0xff151311));
    g.fillRoundedRectangle (cpuTrack, 2.0f);
    g.setColour (juce::Colour (0xff74b84a));
    g.fillRoundedRectangle (cpuTrack.withWidth (cpuTrack.getWidth() * 0.25f), 2.0f);
    g.setColour (LightHostLookAndFeel::textTert2());
    g.setFont (LightHostLookAndFeel::mono (9.0f));
    g.drawText ("CPU", meterX - 30, 0, 26, getHeight(), juce::Justification::centredRight);

    // Settings ⋯ button.
    if (settingsHot)
    {
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRoundedRectangle (settingsRect.toFloat(), 6.0f);
    }
    g.setColour (juce::Colour (0xffa49d92));
    const auto c = settingsRect.getCentre();
    for (int i = -1; i <= 1; ++i)
        g.fillEllipse ((float) c.x + i * 7.0f - 1.75f, (float) c.y - 1.75f, 3.5f, 3.5f);
}

void TitleBar::mouseMove (const juce::MouseEvent& e)
{
    const bool hot = settingsRect.contains (e.getPosition());
    if (hot != settingsHot) { settingsHot = hot; repaint(); }
}

void TitleBar::mouseExit (const juce::MouseEvent&)
{
    if (settingsHot) { settingsHot = false; repaint(); }
}

void TitleBar::mouseDown (const juce::MouseEvent& e)
{
    if (settingsRect.contains (e.getPosition()))
        return showSettingsMenu();

    for (int i = 0; i < tabRects.size(); ++i)
        if (tabRects[i].contains (e.getPosition()))
        {
            if (onSelectPreset) onSelectPreset (i);
            return;
        }

    if (plusRect.contains (e.getPosition()) && onAddPreset)
        onAddPreset();
}

void TitleBar::mouseDoubleClick (const juce::MouseEvent& e)
{
    for (int i = 0; i < tabRects.size(); ++i)
        if (tabRects[i].contains (e.getPosition()))
        {
            if (onRenamePreset) onRenamePreset (i);
            return;
        }
}

void TitleBar::showSettingsMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("LIGHTHOST");
    m.addItem (1, juce::String::fromUTF8 ("Preferences…"));
    m.addItem (2, juce::String::fromUTF8 ("Edit Plug-ins…"));
    m.addSeparator();
    m.addSectionHeader ("PRESET");
    m.addItem (3, "Save Preset");
    m.addItem (4, juce::String::fromUTF8 ("Rename Preset…"));
    m.addItem (5, "Delete Preset");

    settingsHot = true;
    repaint();
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                        .withTargetScreenArea (localAreaToGlobal (settingsRect)),
        [this] (int r)
        {
            settingsHot = false;
            repaint();
            if      (r == 1 && onPreferences) onPreferences();
            else if (r == 2 && onEditPlugins) onEditPlugins();
            else if (r == 3 && onSavePreset)  onSavePreset();
            else if (r == 4 && onRenamePreset && getActivePreset) onRenamePreset (getActivePreset());
            else if (r == 5 && onDeletePreset) onDeletePreset();
        });
}

} // namespace lighthost::ui
