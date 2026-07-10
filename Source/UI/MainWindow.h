//
//  MainWindow.h — top-level editor window. Owned by IconMenu; opened from the
//  tray. Closing it HIDES to the tray (background operation preserved) rather
//  than quitting the app. Carries its own dark LookAndFeel so the global tray /
//  plugin-editor look is untouched.
//

#pragma once

#include <JuceHeader.h>
#include "MainComponent.h"
#include "LightHostLookAndFeel.h"

namespace lighthost::ui
{

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (GraphController& controller, const PresetStore& store, MainComponent::Callbacks callbacks)
        : juce::DocumentWindow ("Light Host",
                                LightHostLookAndFeel::appBg(),
                                juce::DocumentWindow::minimiseButton | juce::DocumentWindow::maximiseButton | juce::DocumentWindow::closeButton)
    {
        setLookAndFeel (&lookAndFeel);
        setUsingNativeTitleBar (true);

        content = new MainComponent (controller, store, std::move (callbacks));
        setContentOwned (content, true);
        setResizable (true, false);
        setResizeLimits (520, 320, 2400, 900);
        centreWithSize (juce::jmin (getWidth(), 1280), getHeight());
    }

    ~MainWindow() override
    {
        clearContentComponent();
        setLookAndFeel (nullptr);
    }

    void refreshChain()   { if (content != nullptr) content->refreshChain(); }
    void refreshPresets() { if (content != nullptr) content->refreshPresets(); }

    // Hide to tray instead of quitting — the engine keeps running in the background.
    void closeButtonPressed() override
    {
        setVisible (false);
    }

private:
    LightHostLookAndFeel lookAndFeel;
    MainComponent* content = nullptr;   // owned by the DocumentWindow content

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

} // namespace lighthost::ui
