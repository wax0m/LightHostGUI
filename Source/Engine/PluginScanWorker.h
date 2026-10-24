//
//  PluginScanWorker.h — scan plugin files in a child process, so a plugin that crashes, hangs or leaves a
//  thread behind during a scan can only take down that child (see ScanProtocol.h for the protocol).
//
//  ScanWorker:          the child side. HostStartup starts one when the command line is a worker's; it scans
//                       its one file on the message thread, prints two lines and exits.
//  OutOfProcessScanner: the host side, a KnownPluginList::CustomScanner. One worker per file, launched only
//                       while a scan runs: nothing exists (no process, thread or timer) otherwise.
//
//  Plain stdout and the process handle are used instead of juce::ChildProcessCoordinator: that one finds out
//  about a dead worker only through its ping timeout (about 9 s), needs the host's message loop to report it, and
//  cannot end a worker whose message thread a plugin has blocked.
//

#pragma once

#include <JuceHeader.h>
#include "ScanProtocol.h"

namespace lighthost::scan
{
    class ScanWorker final
    {
    public:
        // Starts scanning `request` on the message thread (call from initialise()); the process ends by itself
        // after the last line, or after a watchdog interval if a plugin hangs and the host is gone.
        void start (const Request& request);

    private:
        void run (const Request&);
        void send (const juce::String& line);

        juce::AudioPluginFormatManager formatManager;
    };

    struct Timeouts
    {
        int list        = kScanFileTimeoutMs;
        int instantiate = kInstantiateTimeoutMs;
    };

    class OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner
    {
    public:
        explicit OutOfProcessScanner (juce::File workerExecutable = juce::File::getSpecialLocation (juce::File::currentExecutableFile),
                                      Timeouts timeouts = {});

        // false = the worker died or never answered: KnownPluginList blacklists exactly this file.
        bool findPluginTypesFor (juce::AudioPluginFormat&, juce::OwnedArray<juce::PluginDescription>&,
                                 const juce::String& fileOrIdentifier) override;

    private:
        juce::File workerExe;
        Timeouts timeouts;
    };
}
