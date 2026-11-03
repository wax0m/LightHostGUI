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
        if (i == menuTab)   // the tab whose context menu is open
        {
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.fillRoundedRectangle (tabRects[i].toFloat(), 6.0f);
        }
        if (dragging && i == pressedTab)   // the tab being dragged is dimmed
        {
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.fillRoundedRectangle (tabRects[i].toFloat(), 6.0f);
        }
        g.setColour (isActive ? juce::Colour (0xfff0ebe3) : LightHostLookAndFeel::textSecond());
        g.drawText (names[i], tabRects[i], juce::Justification::centred);
        if (isActive)
        {
            g.setColour (LightHostLookAndFeel::accent());
            g.fillRect (tabRects[i].getX() + 6, getHeight() - 2, tabRects[i].getWidth() - 12, 2);
        }
    }

    // Insertion marker while a tab is dragged.
    if (dragging && dropGap >= 0 && ! tabRects.isEmpty())
    {
        const int gx = dropGap < tabRects.size() ? tabRects[dropGap].getX() : tabRects.getLast().getRight();
        g.setColour (LightHostLookAndFeel::accent());
        g.fillRect (gx - 1, 10, 2, getHeight() - 20);
    }

    // "+" add-preset tab.
    g.setColour (LightHostLookAndFeel::textTert());
    g.setFont (LightHostLookAndFeel::display (17.0f, false));
    g.drawText ("+", plusRect, juce::Justification::centred);

    // Right cluster: CPU meter + sample rate (live from the audio device).
    const double sr  = getSampleRate ? getSampleRate() : 0.0;
    const float  cpu = getCpuLoad ? juce::jlimit (0.0f, 1.0f, (float) getCpuLoad()) : 0.0f;

    const int rc = statsClusterBounds().getRight() + 4;
    g.setColour (LightHostLookAndFeel::textTert2());
    g.setFont (LightHostLookAndFeel::mono (10.0f));
    g.drawText (sr > 0.0 ? juce::String (sr / 1000.0, 1) + " kHz" : juce::String ("--"),
                rc - 66, 0, 62, getHeight(), juce::Justification::centredRight);

    const int meterX = rc - 66 - 8 - 46;
    juce::Rectangle<float> cpuTrack ((float) meterX, getHeight() * 0.5f - 2.5f, 46.0f, 5.0f);
    g.setColour (juce::Colour (0xff151311));
    g.fillRoundedRectangle (cpuTrack, 2.0f);
    g.setColour (cpu > 0.85f ? LightHostLookAndFeel::meterRed() : LightHostLookAndFeel::meterGreen());
    g.fillRoundedRectangle (cpuTrack.withWidth (cpuTrack.getWidth() * cpu), 2.0f);
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

// From the "CPU" label's left edge to the right edge of the kHz text. paint() anchors the
// cluster on this rectangle (rc = right + 4), so a width change is made in one place.
juce::Rectangle<int> TitleBar::statsClusterBounds() const
{
    const int rc   = settingsRect.getX() - 12;
    const int left = rc - 66 - 8 - 46 - 30;
    return { left, 0, (rc - 4) - left, getHeight() };
}

// Keeps the periodic stats tick from re-rasterising the gradient/logo/preset tabs.
void TitleBar::repaintStats()
{
    repaint (statsClusterBounds());
}

int TitleBar::tabAt (juce::Point<int> p) const
{
    for (int i = 0; i < tabRects.size(); ++i)
        if (tabRects[i].contains (p))
            return i;
    return -1;
}

juce::String TitleBar::getTooltip()
{
    if (hoverTab < 0 || ! getPresetShortcut || ! (shortcutsAvailable && shortcutsAvailable()))
        return {};
    const auto text = getPresetShortcut (hoverTab);
    return text.isEmpty() ? juce::String() : "Shortcut: " + text;
}

void TitleBar::mouseMove (const juce::MouseEvent& e)
{
    hoverTab = tabAt (e.getPosition());
    const bool hot = settingsRect.contains (e.getPosition());
    if (hot != settingsHot) { settingsHot = hot; repaint(); }
}

void TitleBar::mouseExit (const juce::MouseEvent&)
{
    hoverTab = -1;
    if (settingsHot) { settingsHot = false; repaint(); }
}

void TitleBar::mouseDown (const juce::MouseEvent& e)
{
    // Right click opens the menu of the tab under it and never switches presets.
    if (e.mods.isPopupMenu())
    {
        for (int i = 0; i < tabRects.size(); ++i)
            if (tabRects[i].contains (e.getPosition()))
                return showTabMenu (i);
        return;
    }
    if (! e.mods.isLeftButtonDown())
        return;

    if (settingsRect.contains (e.getPosition()))
        return showSettingsMenu();

    pressedTab = -1;
    dragging = false;
    dropGap = -1;
    for (int i = 0; i < tabRects.size(); ++i)
        if (tabRects[i].contains (e.getPosition()))
        {
            pressedTab = i;   // selected on release, unless this turns into a drag
            return;
        }

    if (plusRect.contains (e.getPosition()) && onAddPreset)
        onAddPreset();
}

// Insertion slot (0..n): before the first tab whose centre is right of x.
int TitleBar::gapAt (int x) const
{
    for (int i = 0; i < tabRects.size(); ++i)
        if (x < tabRects[i].getCentreX())
            return i;
    return tabRects.size();
}

void TitleBar::mouseDrag (const juce::MouseEvent& e)
{
    if (pressedTab < 0 || ! e.mods.isLeftButtonDown())
        return;
    if (! dragging && e.getDistanceFromDragStart() < dragThreshold)
        return;

    dragging = true;
    const int gap = gapAt (e.getPosition().x);
    if (gap != dropGap)
    {
        dropGap = gap;
        repaint();
    }
}

void TitleBar::mouseUp (const juce::MouseEvent& e)
{
    const int from = pressedTab;
    const bool wasDragging = dragging;
    const int gap = dropGap;
    pressedTab = -1;
    dragging = false;
    dropGap = -1;
    if (from < 0)
        return;

    if (! wasDragging)
    {
        if (onSelectPreset) onSelectPreset (from);   // a plain click
        return;
    }

    repaint();
    const bool insideStrip = e.getPosition().y >= 0 && e.getPosition().y < getHeight();   // released elsewhere = cancelled
    const int to = gap > from ? gap - 1 : gap;
    if (insideStrip && gap >= 0 && to != from && onMovePreset)
        onMovePreset (from, to);
}

void TitleBar::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! e.mods.isLeftButtonDown())
        return;

    for (int i = 0; i < tabRects.size(); ++i)
        if (tabRects[i].contains (e.getPosition()))
        {
            if (onRenamePreset) onRenamePreset (i);
            return;
        }
}

void TitleBar::showTabMenu (int tab)
{
    const bool isActive = getActivePreset && tab == getActivePreset();

    juce::PopupMenu m;
    m.addItem (1, "Save", isActive);   // saving snapshots the live graph, which is the active preset
    m.addItem (2, juce::String::fromUTF8 ("Rename…"));
    m.addItem (3, "Duplicate");
    m.addSeparator();
    m.addItem (5, "Move Left", tab > 0);
    m.addItem (6, "Move Right", tab < tabRects.size() - 1);
    if (shortcutsAvailable && shortcutsAvailable())
    {
        const auto current = getPresetShortcut ? getPresetShortcut (tab) : juce::String();
        m.addSeparator();
        juce::PopupMenu::Item set (juce::String::fromUTF8 ("Set Shortcut…"));
        set.itemID = 7;
        set.shortcutKeyDescription = current;   // shown right-aligned in the menu
        m.addItem (set);
        m.addItem (8, "Clear Shortcut", current.isNotEmpty());
    }
    m.addSeparator();
    m.addItem (4, "Delete");

    menuTab = tab;
    repaint();
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                        .withTargetScreenArea (localAreaToGlobal (tabRects[tab])),
        [safe = juce::Component::SafePointer<TitleBar> (this), tab] (int r)
        {
            if (safe == nullptr)
                return;
            safe->menuTab = -1;
            safe->repaint();
            if      (r == 1 && safe->onSavePreset)      safe->onSavePreset();
            else if (r == 2 && safe->onRenamePreset)    safe->onRenamePreset (tab);
            else if (r == 3 && safe->onDuplicatePreset) safe->onDuplicatePreset (tab);
            else if (r == 4 && safe->onDeletePreset)    safe->onDeletePreset (tab);
            else if (r == 5 && safe->onMovePreset)      safe->onMovePreset (tab, tab - 1);
            else if (r == 6 && safe->onMovePreset)      safe->onMovePreset (tab, tab + 1);
            else if (r == 7 && safe->onSetShortcut)     safe->onSetShortcut (tab);
            else if (r == 8 && safe->onClearShortcut)   safe->onClearShortcut (tab);
        });
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
    m.addItem (6, "Duplicate Preset");
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
            else if (r == 6 && onDuplicatePreset && getActivePreset) onDuplicatePreset (getActivePreset());
            else if (r == 5 && onDeletePreset && getActivePreset) onDeletePreset (getActivePreset());
        });
}

} // namespace lighthost::ui
