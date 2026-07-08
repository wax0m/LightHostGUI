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
    explicit MainWindow (GraphController& controller)
        : juce::DocumentWindow ("Light Host",
                                LightHostLookAndFeel::bg(),
                                juce::DocumentWindow::minimiseButton | juce::DocumentWindow::closeButton)
    {
        setLookAndFeel (&lookAndFeel);
        setUsingNativeTitleBar (true);
        setContentOwned (new MainComponent (controller), true);
        setResizable (true, false);
        setResizeLimits (360, 300, 1200, 900);
        centreWithSize (getWidth(), getHeight());
    }

    ~MainWindow() override
    {
        clearContentComponent();
        setLookAndFeel (nullptr);
    }

    // Hide to tray instead of quitting — the engine keeps running in the background.
    void closeButtonPressed() override
    {
        setVisible (false);
    }

private:
    LightHostLookAndFeel lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

} // namespace lighthost::ui
