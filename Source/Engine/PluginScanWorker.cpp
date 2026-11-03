#include "PluginScanWorker.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <fcntl.h>
 #include <unistd.h>
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

       #if JUCE_WINDOWS
        HANDLE protocolHandle = nullptr;   // the host's pipe, duplicated before stdout is pointed at NUL
       #else
        int protocolFd = -1;
       #endif

        // C-R1: from here on whatever a plugin prints (printf, std::cout, WriteFile on the standard handle,
        // stderr) goes to NUL, and only send() reaches the host, through a private duplicate of the pipe.
        void detachPluginOutput()
        {
           #if JUCE_WINDOWS
            HANDLE own = nullptr;
            const HANDLE out = GetStdHandle (STD_OUTPUT_HANDLE);
            if (out == nullptr || out == INVALID_HANDLE_VALUE
                || ! DuplicateHandle (GetCurrentProcess(), out, GetCurrentProcess(), &own, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return;   // no pipe to protect

            protocolHandle = own;
            const HANDLE nul = CreateFileW (L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (nul != INVALID_HANDLE_VALUE)
            {
                SetStdHandle (STD_OUTPUT_HANDLE, nul);
                SetStdHandle (STD_ERROR_HANDLE, nul);
            }
            FILE* unused = nullptr;
            freopen_s (&unused, "NUL", "w", stdout);
            freopen_s (&unused, "NUL", "w", stderr);
           #else
            protocolFd = dup (STDOUT_FILENO);
            const int devNull = open ("/dev/null", O_WRONLY);
            if (protocolFd >= 0 && devNull >= 0)
            {
                dup2 (devNull, STDOUT_FILENO);
                dup2 (devNull, STDERR_FILENO);
            }
            if (devNull >= 0)
                close (devNull);
           #endif
        }

        // A stand-in for a plugin that prints and starts helper processes (test modes only).
        void makeNoise()
        {
            std::printf ("plugin banner on stdout\n");
            std::fflush (stdout);
            std::fprintf (stderr, "plugin warning on stderr\n");
           #if JUCE_WINDOWS
            const char raw[] = "raw write to the standard handle\n";
            DWORD written = 0;
            WriteFile (GetStdHandle (STD_OUTPUT_HANDLE), raw, (DWORD) (sizeof (raw) - 1), &written, nullptr);
           #endif
        }

        void spawnHelperAndRecordPid()
        {
           #if JUCE_WINDOWS
            wchar_t command[] = L"cmd.exe /c ping -n 60 127.0.0.1 > NUL";
            STARTUPINFOW startup {};
            startup.cb = sizeof (startup);
            PROCESS_INFORMATION info {};
            if (CreateProcessW (nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
            {
                const auto pidFile = SystemStats::getEnvironmentVariable (kTestPidFileEnvVar, {});
                if (pidFile.isNotEmpty())
                    File (pidFile).replaceWithText (String ((int) info.dwProcessId));
                CloseHandle (info.hThread);
                CloseHandle (info.hProcess);
            }
           #endif
        }
    }

    void ScanWorker::send (const String& line)
    {
        const auto utf8 = line.toStdString();
       #if JUCE_WINDOWS
        DWORD written = 0;
        WriteFile (protocolHandle != nullptr ? protocolHandle : GetStdHandle (STD_OUTPUT_HANDLE),
                   utf8.data(), (DWORD) utf8.size(), &written, nullptr);
       #else
        const int fd = protocolFd >= 0 ? protocolFd : STDOUT_FILENO;
        for (size_t done = 0; done < utf8.size();)
        {
            const auto n = write (fd, utf8.data() + done, utf8.size() - done);
            if (n <= 0)
                break;
            done += (size_t) n;
        }
       #endif
    }

    void ScanWorker::start (const Request& request)
    {
        detachPluginOutput();   // before any plugin code can run
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
        if (testMode == "spawn-child") { spawnHelperAndRecordPid(); hangForever(); }
        if (testMode == "noise")          makeNoise();
        if (testMode == "noise-protocol") send ("some plugin printed this\n@LHSCAN2 not ours\n");

        AudioPluginFormat* format = nullptr;
        for (auto* f : formatManager.getFormats())
            if (f->getName() == request.formatName)
                format = f;

        OwnedArray<PluginDescription> found;
        if (format != nullptr)
            format->findAllTypesForFile (found, request.fileOrIdentifier);

        send (encodeListLine (found));   // line 1: the host can already use the descriptions

        if (testMode == "noise")          makeNoise();
        if (testMode == "noise-protocol") send ("more noise between the lines\n");

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
        // One launched worker: a reader thread turns its stdout into protocol lines and notices when the process
        // is gone. Windows: the worker runs in a Job object that kills the whole process tree when it closes
        // (C-R3: a plugin's own helper processes, e.g. SteinbergMediaBayServer.exe, must not outlive a killed
        // worker). The process is always ended before the reader is joined.
        class Worker
        {
        public:
            ~Worker()
            {
                killTree();
                if (reader.joinable())
                {
                   #if JUCE_WINDOWS
                    CancelSynchronousIo (reader.native_handle());   // a process that escaped the job may still hold the pipe
                   #endif
                    reader.join();
                }
                closeHandles();
            }

            bool launch (const StringArray& commandLine)
            {
                if (! startProcess (commandLine))
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
           #if JUCE_WINDOWS
            // One argument the way CommandLineToArgvW reads it back.
            static String quoteArgument (const String& a)
            {
                // Plain arguments stay bare: the worker recognises its flag in the raw command line text.
                if (a.isNotEmpty() && ! a.containsAnyOf (" \t\""))
                    return a;

                String out = "\"";
                int backslashes = 0;
                for (auto c : a)
                {
                    if (c == '\\')
                    {
                        ++backslashes;
                        continue;
                    }
                    if (c == '"')
                        out += String::repeatedString ("\\", backslashes * 2 + 1);   // the backslashes and the quote
                    else
                        out += String::repeatedString ("\\", backslashes);
                    backslashes = 0;
                    out += String::charToString (c);
                }
                return out + String::repeatedString ("\\", backslashes * 2) + "\"";   // before the closing quote
            }

            bool startProcess (const StringArray& commandLine)
            {
                SECURITY_ATTRIBUTES inheritable { sizeof (SECURITY_ATTRIBUTES), nullptr, TRUE };
                HANDLE pipeRead = nullptr, pipeWrite = nullptr;
                if (! CreatePipe (&pipeRead, &pipeWrite, &inheritable, 0))
                    return false;
                SetHandleInformation (pipeRead, HANDLE_FLAG_INHERIT, 0);   // only the worker's end is inherited

                HANDLE nul = CreateFileW (L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          &inheritable, OPEN_EXISTING, 0, nullptr);

                String joined;
                for (const auto& a : commandLine)
                    joined += (joined.isEmpty() ? "" : " ") + quoteArgument (a);
                std::vector<wchar_t> buffer (joined.toWideCharPointer(), joined.toWideCharPointer() + joined.length() + 1);

                STARTUPINFOW startup {};
                startup.cb = sizeof (startup);
                startup.dwFlags = STARTF_USESTDHANDLES;
                startup.hStdInput  = nul;
                startup.hStdOutput = pipeWrite;
                startup.hStdError  = nul;

                PROCESS_INFORMATION info {};
                const BOOL started = CreateProcessW (nullptr, buffer.data(), nullptr, nullptr, TRUE,
                                                     CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);

                // The parent's copy of the write end must go, or the pipe never reports the worker's death.
                CloseHandle (pipeWrite);
                if (nul != INVALID_HANDLE_VALUE)
                    CloseHandle (nul);

                if (! started)
                {
                    CloseHandle (pipeRead);
                    return false;
                }

                job = CreateJobObjectW (nullptr, nullptr);
                if (job != nullptr)
                {
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
                    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                    if (! SetInformationJobObject (job, JobObjectExtendedLimitInformation, &limits, sizeof (limits))
                        || ! AssignProcessToJobObject (job, info.hProcess))
                    {
                        CloseHandle (job);   // still killed through its process handle, just not its children
                        job = nullptr;
                    }
                }

                processHandle = info.hProcess;
                readPipe = pipeRead;
                ResumeThread (info.hThread);
                CloseHandle (info.hThread);
                return true;
            }

            void killTree()
            {
                if (job != nullptr)
                {
                    CloseHandle (job);   // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: the worker and everything it started
                    job = nullptr;
                }
                if (processHandle != nullptr)
                    TerminateProcess (processHandle, 1);
            }

            void closeHandles()
            {
                if (processHandle != nullptr) { CloseHandle (processHandle); processHandle = nullptr; }
                if (readPipe != nullptr)      { CloseHandle (readPipe);      readPipe = nullptr; }
            }

            // False once the process has ended and its pipe is drained.
            bool readByte (char& byte)
            {
                DWORD got = 0;
                return ReadFile (readPipe, &byte, 1, &got, nullptr) && got == 1;
            }

            HANDLE readPipe = nullptr, processHandle = nullptr, job = nullptr;
           #else
            bool startProcess (const StringArray& commandLine) { return process.start (commandLine, ChildProcess::wantStdOut); }
            void killTree() { process.kill(); }
            void closeHandles() {}
            bool readByte (char& byte) { return process.readProcessOutput (&byte, 1) == 1; }

            ChildProcess process;
           #endif

            void readLoop()
            {
                LineBuffer buffer;
                char byte = 0;

                while (readByte (byte))
                {
                    const auto complete = buffer.add (&byte, 1);
                    if (complete.empty())
                        continue;

                    const std::lock_guard<std::mutex> lock { mutex };
                    for (const auto& l : complete)
                        if (isProtocolLine (l))   // whatever else reached the pipe is noise (C-R1)
                            lines.push_back (l);
                    condvar.notify_all();
                }

                const std::lock_guard<std::mutex> lock { mutex };
                gone = true;
                condvar.notify_all();
            }

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
