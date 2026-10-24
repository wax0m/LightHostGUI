#include "PluginScanWorker.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

using namespace juce;

namespace lighthost::scan
{
    //==========================================================================
    // Worker side
    //==========================================================================
    namespace
    {
        [[noreturn]] void hangForever()
        {
            for (;;)
                Thread::sleep (1000);
        }
    }

    void ScanWorker::send (const String& line)
    {
        const auto utf8 = line.toStdString();
       #if JUCE_WINDOWS
        DWORD written = 0;
        WriteFile (GetStdHandle (STD_OUTPUT_HANDLE), utf8.data(), (DWORD) utf8.size(), &written, nullptr);
       #else
        std::fwrite (utf8.data(), 1, utf8.size(), stdout);
        std::fflush (stdout);
       #endif
    }

    void ScanWorker::start (const Request& request)
    {
       #if JUCE_WINDOWS
        // A plugin that crashes in here must end the worker quietly, without a Windows error dialog to dismiss.
        SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
       #endif
        addDefaultFormatsToManager (formatManager);

        // The host kills a worker that hangs; this only covers a host that died meanwhile.
        std::thread ([] { std::this_thread::sleep_for (std::chrono::milliseconds (kScanFileTimeoutMs + kInstantiateTimeoutMs + 15000));
                          std::_Exit (4); }).detach();

        MessageManager::callAsync ([this, request]
        {
            run (request);
            std::_Exit (0);   // nothing to save, and a plugin's leftover threads must not keep the process alive
        });
    }

    void ScanWorker::run (const Request& request)
    {
        const auto testMode = testModeFromEnvironment();

        if (testMode == "abort")  std::_Exit (3);   // dies without a result, like a crashing plugin
        if (testMode == "hang")   hangForever();

        AudioPluginFormat* format = nullptr;
        for (auto* f : formatManager.getFormats())
            if (f->getName() == request.formatName)
                format = f;

        OwnedArray<PluginDescription> found;
        if (format != nullptr)
            format->findAllTypesForFile (found, request.fileOrIdentifier);

        send (encodeListLine (found));   // line 1: the host can already use the descriptions

        if (testMode == "hang-instantiate")
            hangForever();

        // One test load of every description, so a plugin that dies on instantiation (HALion Sonic) is found
        // here, in the worker, and not when the user adds it to the graph.
        bool ok = true;
        for (const auto* d : found)
        {
            String error;
            if (auto instance = formatManager.createPluginInstance (*d, 48000.0, 512, error))
            {
                instance->prepareToPlay (48000.0, 512);
                instance->releaseResources();
            }
            else
            {
                ok = false;
            }
        }

        send (encodeInstantiatedLine (ok));   // line 2
    }

    //==========================================================================
    // Host side
    //==========================================================================
    namespace
    {
        // One launched worker: a reader thread turns its stdout into lines and notices when the process is gone.
        // The process is always ended (TerminateProcess) before the reader is joined.
        class Worker
        {
        public:
            ~Worker()
            {
                process.kill();
                if (reader.joinable())
                    reader.join();
            }

            bool launch (const StringArray& commandLine)
            {
                if (! process.start (commandLine, ChildProcess::wantStdOut))
                    return false;

                reader = std::thread ([this] { readLoop(); });
                return true;
            }

            struct Reply
            {
                Event event = Event::timeout;
                String line;
                bool cancelled = false;   // the scan was abandoned while waiting
            };

            Reply wait (int timeoutMs, const std::function<bool()>& shouldExit)
            {
                const auto deadline = Time::getMillisecondCounter() + (uint32) timeoutMs;
                std::unique_lock<std::mutex> lock { mutex };

                for (;;)
                {
                    if (! lines.empty())
                    {
                        Reply r { Event::reply, lines.front(), false };
                        lines.pop_front();
                        return r;
                    }

                    if (gone)
                        return { Event::workerGone, {}, false };

                    if (shouldExit())
                        return { Event::timeout, {}, true };

                    if (Time::getMillisecondCounter() >= deadline)
                        return { Event::timeout, {}, false };

                    condvar.wait_for (lock, std::chrono::milliseconds (25));
                }
            }

        private:
            void readLoop()
            {
                LineBuffer buffer;
                char byte = 0;

                // Returns 0 once the process has ended and its pipe is drained.
                while (process.readProcessOutput (&byte, 1) == 1)
                {
                    const auto complete = buffer.add (&byte, 1);
                    if (complete.empty())
                        continue;

                    const std::lock_guard<std::mutex> lock { mutex };
                    for (const auto& l : complete)
                        lines.push_back (l);
                    condvar.notify_all();
                }

                const std::lock_guard<std::mutex> lock { mutex };
                gone = true;
                condvar.notify_all();
            }

            ChildProcess process;
            std::thread reader;
            std::mutex mutex;
            std::condition_variable condvar;
            std::deque<String> lines;
            bool gone = false;
        };
    }

    OutOfProcessScanner::OutOfProcessScanner (File exe, Timeouts t) : workerExe (std::move (exe)), timeouts (t) {}

    bool OutOfProcessScanner::findPluginTypesFor (AudioPluginFormat& format, OwnedArray<PluginDescription>& result,
                                                  const String& fileOrIdentifier)
    {
        Worker worker;

        if (! worker.launch (workerCommandLine (workerExe, { format.getName(), fileOrIdentifier })))
        {
            // Not the plugin's fault: do not blacklist it for a failure of ours.
            Logger::writeToLog ("Plugin scan: could not start the scan worker for " + fileOrIdentifier);
            return true;
        }

        const auto cancelled = [this] { return shouldExit(); };

        const auto list = worker.wait (timeouts.list, cancelled);
        if (list.cancelled)
            return true;

        std::optional<std::vector<PluginDescription>> descriptions;
        if (list.event == Event::reply)
            descriptions = decodeListLine (list.line);

        if (verdictForListPhase (list.event) == Verdict::blacklist || ! descriptions.has_value())
        {
            Logger::writeToLog ("Plugin scan: " + fileOrIdentifier + " did not answer (worker "
                                + (list.event == Event::workerGone ? "died" : "timed out") + "); blacklisted");
            return false;
        }

        const auto test = worker.wait (timeouts.instantiate, cancelled);
        if (test.cancelled)
            return true;

        switch (verdictForInstantiatePhase (test.event))
        {
            case Verdict::blacklist:
                Logger::writeToLog ("Plugin scan: " + fileOrIdentifier + " died during its test load; blacklisted");
                return false;

            case Verdict::acceptWithoutBlacklist:
                Logger::writeToLog ("Plugin scan: the test load of " + fileOrIdentifier + " did not finish; kept, not blacklisted");
                break;

            case Verdict::accept:
                if (const auto ok = decodeInstantiatedLine (test.line); ok.has_value() && ! *ok)
                    Logger::writeToLog ("Plugin scan: " + fileOrIdentifier + " could not be instantiated; kept, not blacklisted");
                break;
        }

        for (const auto& d : *descriptions)
            result.add (new PluginDescription (d));

        return true;
    }
}
