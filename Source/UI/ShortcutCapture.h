//
//  ShortcutCapture.h — the "Set Shortcut…" dialog: shows the combination as it is pressed, OK takes it, Esc / Cancel /
//  the close button leave the old one. onFinished fires exactly once, on every way out (a combination, or nullopt).
//  Used modally by IconMenu, which turns its own hotkeys off while the dialog is open.
//

#pragma once

#include <JuceHeader.h>
#include "LightHostLookAndFeel.h"
#include "../Engine/HotkeyCombo.h"
#include "../PresetHotkeys.h"

namespace lighthost::ui
{

class ShortcutCaptureContent : public juce::Component
{
public:
    using Result = std::optional<lighthost::hotkeys::Combo>;
    std::function<void (Result)> onFinished;

    ShortcutCaptureContent (const juce::String& presetName, const juce::String& currentText)
        : prompt (presetName), current (currentText)
    {
        setWantsKeyboardFocus (true);
        for (auto* b : { &okButton, &cancelButton })
        {
            addAndMakeVisible (*b);
            b->setWantsKeyboardFocus (false);   // every key, Return and Space included, belongs to the capture
            b->setMouseClickGrabsKeyboardFocus (false);
        }
        okButton.setEnabled (false);
        okButton.onClick     = [this] { finish (captured); };
        cancelButton.onClick = [this] { finish (std::nullopt); };
        setSize (380, 150);
    }

    void cancel() { finish (std::nullopt); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (LightHostLookAndFeel::menuSurface());
        g.setColour (LightHostLookAndFeel::textSecond());
        g.setFont (LightHostLookAndFeel::display (13.0f, false));
        g.drawText (juce::String::fromUTF8 ("Shortcut for \"") + prompt + "\"", 16, 12, getWidth() - 32, 20, juce::Justification::centredLeft);

        auto box = juce::Rectangle<float> (16.0f, 40.0f, (float) getWidth() - 32.0f, 44.0f);
        g.setColour (LightHostLookAndFeel::meterWell());
        g.fillRoundedRectangle (box, 6.0f);
        g.setColour (LightHostLookAndFeel::menuBorder());
        g.drawRoundedRectangle (box.reduced (0.5f), 6.0f, 1.0f);

        g.setColour (captured.has_value() ? LightHostLookAndFeel::textPrimary() : LightHostLookAndFeel::textTert());
        g.setFont (LightHostLookAndFeel::mono (15.0f));
        g.drawText (captured.has_value() ? lighthost::hotkeys::format (*captured)
                                         : (current.isNotEmpty() ? current : juce::String::fromUTF8 ("Press a key combination…")),
                    box.toNearestInt(), juce::Justification::centred);

        if (hint.isNotEmpty())
        {
            g.setColour (LightHostLookAndFeel::meterAmber());
            g.setFont (LightHostLookAndFeel::display (12.0f, false));
            g.drawText (hint, 16, 88, getWidth() - 32, 16, juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16).removeFromBottom (28);
        cancelButton.setBounds (r.removeFromRight (90));
        r.removeFromRight (8);
        okButton.setBounds (r.removeFromRight (90));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress (juce::KeyPress::escapeKey))
        {
            cancel();
            return true;
        }

        const auto combo = lighthost::hotkeys::fromKeyPress (key, PresetHotkeys::isWinKeyDown(), PresetHotkeys::pressedKeyToken());
        if (! combo.has_value())
            hint = "That key is not supported.";
        else if (! combo->hasRequiredModifier())
            hint = "Use Ctrl, Alt or Win with the key.";
        else
            hint = lighthost::hotkeys::altGrWarning (*combo);

        captured = (combo.has_value() && combo->hasRequiredModifier()) ? combo : std::nullopt;
        okButton.setEnabled (captured.has_value());
        repaint();
        return true;
    }

    bool keyStateChanged (bool) override { return true; }

private:
    void finish (Result result)
    {
        if (done)
            return;
        done = true;
        if (onFinished)
            onFinished (std::move (result));
    }

    juce::String prompt, current, hint;
    Result captured;
    bool done = false;
    juce::TextButton okButton { "OK" }, cancelButton { "Cancel" };
};

// The dialog window; closing it by any route (title-bar X, Alt+F4) counts as Cancel.
class ShortcutCaptureWindow : public juce::DialogWindow
{
public:
    ShortcutCaptureWindow (const juce::String& presetName, const juce::String& currentText,
                           std::function<void (ShortcutCaptureContent::Result)> finished)
        : juce::DialogWindow ("Set Shortcut", LightHostLookAndFeel::menuSurface(), true, true)
    {
        auto* content = new ShortcutCaptureContent (presetName, currentText);
        content->onFinished = std::move (finished);
        capture = content;
        setContentOwned (content, true);
        setUsingNativeTitleBar (true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        enterModalState (true, nullptr, false);   // nothing else can change the presets while this is open
        content->grabKeyboardFocus();
    }

    void closeButtonPressed() override
    {
        if (capture != nullptr)
            capture->cancel();
    }

private:
    juce::Component::SafePointer<ShortcutCaptureContent> capture;
};

} // namespace lighthost::ui
