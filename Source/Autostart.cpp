#include "Autostart.h"

namespace lighthost::autostart
{

#if JUCE_WINDOWS
static juce::String runKey()
{
    return "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\" + kRunValueName;
}

static juce::String thisExePath()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
}

State read()
{
    return classify (juce::WindowsRegistry::getValue (runKey()), thisExePath());
}

bool enableForThisExe()
{
    return juce::WindowsRegistry::setValue (runKey(), makeRunValue (thisExePath()));
}

bool disable()
{
    return juce::WindowsRegistry::deleteValue (runKey());
}
#else
State read()             { return State::off; }
bool  enableForThisExe() { return false; }
bool  disable()          { return false; }
#endif

} // namespace lighthost::autostart
