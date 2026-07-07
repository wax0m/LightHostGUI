#include <JuceHeader.h>
#include "IconMenu.hpp"
#include "Engine/SelfTest.h"

#if JUCE_WINDOWS
 #include <windows.h>   // AttachConsole for -run-selftest console output
#endif

#if ! (JUCE_PLUGINHOST_VST || JUCE_PLUGINHOST_VST3 || JUCE_PLUGINHOST_AU)
 #error "If you're building the audio plugin host, you probably want to enable VST and/or AU support"
#endif

class PluginHostApp  : public JUCEApplication
{
public:
    PluginHostApp() {}

    void initialise (const String&) override
    {
        // Headless self-test: verify the real binary's engine end-to-end (no tray,
        // no GUI) and exit with 0/1 so CI can gate on it.
        //   "Light Host" -run-selftest [plugin.vst3 ...]
        if (getCommandLineParameterArray().contains ("-run-selftest"))
        {
            runSelfTestAndQuit();
            return;
        }

        PropertiesFile::Options options;
        options.applicationName     = getApplicationName();
        options.filenameSuffix      = "settings";
        options.osxLibrarySubFolder = "Preferences";

        checkArguments (&options);

        appProperties = std::make_unique<ApplicationProperties>();
        appProperties->setStorageParameters (options);

        LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        mainWindow = std::make_unique<IconMenu>();
        #if JUCE_MAC
        Process::setDockIconVisible (false);
        #endif
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        appProperties = nullptr;
        LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        JUCEApplicationBase::quit();
    }

    const String getApplicationName() override       { return "Light Host"; }
    const String getApplicationVersion() override    { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override
    {
        StringArray multiInstance = getParameter ("-multi-instance");
        return multiInstance.size() == 2;
    }

    ApplicationCommandManager commandManager;
    std::unique_ptr<ApplicationProperties> appProperties;
    LookAndFeel_V3 lookAndFeel;

private:
    std::unique_ptr<IconMenu> mainWindow;

    void runSelfTestAndQuit()
    {
       #if JUCE_WINDOWS
        // GUI-subsystem exe has no console of its own; attach to the launching
        // shell's so the self-test report is visible when run from a terminal/CI.
        if (AttachConsole (ATTACH_PARENT_PROCESS))
        {
            FILE* fp = nullptr;
            freopen_s (&fp, "CONOUT$", "w", stdout);
            freopen_s (&fp, "CONOUT$", "w", stderr);
        }
       #endif

        // Any argument that isn't a switch is treated as an explicit plugin path.
        StringArray pluginPaths;
        for (const auto& arg : getCommandLineParameterArray())
            if (! arg.startsWith ("-"))
                pluginPaths.add (arg.unquoted());

        const int result = lighthost::runSelfTest (pluginPaths);
        setApplicationReturnValue (result);
        quit();
    }

    StringArray getParameter (String lookFor)
    {
        StringArray parameters = getCommandLineParameterArray();
        StringArray found;
        for (int i = 0; i < parameters.size(); ++i)
        {
            String param = parameters[i];
            if (param.contains (lookFor))
            {
                found.add (lookFor);
                int delimiter = param.indexOf (0, "=") + 1;
                String val = param.substring (delimiter);
                found.add (val);
                return found;
            }
        }
        return found;
    }

    void checkArguments (PropertiesFile::Options* options)
    {
        StringArray multiInstance = getParameter ("-multi-instance");
        if (multiInstance.size() == 2)
            options->filenameSuffix = multiInstance[1] + "." + options->filenameSuffix;
    }
};

static PluginHostApp& getApp()                      { return *dynamic_cast<PluginHostApp*> (JUCEApplication::getInstance()); }
ApplicationCommandManager& getCommandManager()      { return getApp().commandManager; }
ApplicationProperties& getAppProperties()           { return *getApp().appProperties; }

START_JUCE_APPLICATION (PluginHostApp)
