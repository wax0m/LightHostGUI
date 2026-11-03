//
//  ScanProtocol.h — the pure part of out-of-process plugin scanning: the worker's command line, its
//  reply lines on stdout, and the decision table for what a worker's behaviour means.
//  Header-only and headless-testable; PluginScanWorker.{h,cpp} add the processes around it.
//
//  One worker scans ONE plugin file (so a leftover thread of file A can never kill the scan of file B):
//    host   -> worker : command line   <exe> --lighthostscan <format> <file>
//    worker -> host   : line 1  "L <LIST xml>"      the descriptions, right after findAllTypesForFile
//    worker -> host   : line 2  "I 1" or "I 0"      after one test load of every description
//  The host reads stdout and watches the process handle: a dead worker is seen at once, a hung one is killed.
//

#pragma once

#include <JuceHeader.h>
#include <optional>
#include <vector>

namespace lighthost::scan
{
    // First argument of a scan worker's command line.
    inline const juce::String kWorkerFlag = "--lighthostscan";

    // Waiting for a worker. Named constants, passed as parameters in the tests.
    constexpr int kScanFileTimeoutMs    = 30000;  // until line 1: loading the file and listing its plugins
    constexpr int kInstantiateTimeoutMs = 30000;  // until line 2: the test load (a plugin may show a modal dialog)

    // Test-only fault injection: LIGHTHOST_SCAN_WORKER_TEST in the environment of the worker.
    //   abort             worker dies before line 1
    //   hang              worker never answers
    //   hang-instantiate  worker sends line 1, then never finishes the test load
    //   noise             a "plugin" prints to stdout / stderr before and between the lines (the worker must swallow it)
    //   noise-protocol    junk lines arrive on the protocol channel itself (the host must skip them)
    //   spawn-child       worker starts a helper process (its pid goes to the file in kTestPidFileEnvVar), then hangs
    inline const char* const kTestEnvVar = "LIGHTHOST_SCAN_WORKER_TEST";
    inline const char* const kTestPidFileEnvVar = "LIGHTHOST_SCAN_WORKER_PIDFILE";

    // Every protocol line starts with this. A plugin that prints to stdout cannot be told apart from the
    // worker by position, so the host keeps only lines that carry it and skips the rest (C-R1).
    inline const juce::String kLinePrefix = "@LHSCAN1 ";

    inline bool isProtocolLine (const juce::String& line) { return line.startsWith (kLinePrefix); }

    //==========================================================================
    struct Request
    {
        juce::String formatName, fileOrIdentifier;
    };

    // The whole command line of a worker for a request.
    inline juce::StringArray workerCommandLine (const juce::File& exe, const Request& r)
    {
        return { exe.getFullPathName(), kWorkerFlag, r.formatName, r.fileOrIdentifier };
    }

    // True for the command line a worker is started with (the string JUCE hands to initialise()).
    inline bool isWorkerCommandLine (const juce::String& commandLine)
    {
        const auto t = commandLine.trim();
        return t == kWorkerFlag || t.startsWith (kWorkerFlag + " ");
    }

    // The parameters (command line without the exe) -> the request; nullopt when anything is missing.
    inline std::optional<Request> parseWorkerParameters (const juce::StringArray& params)
    {
        if (params.size() != 3 || params[0] != kWorkerFlag || params[1].isEmpty() || params[2].isEmpty())
            return std::nullopt;

        return Request { params[1], params[2] };
    }

    //==========================================================================
    // Line 1.
    inline juce::String encodeListLine (const juce::OwnedArray<juce::PluginDescription>& found)
    {
        juce::XmlElement xml ("LIST");
        for (const auto* d : found)
            xml.addChildElement (d->createXml().release());
        return kLinePrefix + "L " + xml.toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()) + "\n";
    }

    // nullopt when the line is not a list line. Entries that do not parse are skipped.
    inline std::optional<std::vector<juce::PluginDescription>> decodeListLine (const juce::String& line)
    {
        if (! line.startsWith (kLinePrefix + "L "))
            return std::nullopt;

        const auto xml = juce::parseXML (line.substring (kLinePrefix.length() + 2));
        if (xml == nullptr || ! xml->hasTagName ("LIST"))
            return std::nullopt;

        std::vector<juce::PluginDescription> out;
        for (const auto* item : xml->getChildIterator())
        {
            juce::PluginDescription d;
            if (d.loadFromXml (*item))
                out.push_back (std::move (d));
        }
        return out;
    }

    // Line 2.
    inline juce::String encodeInstantiatedLine (bool ok)
    {
        return kLinePrefix + "I " + (ok ? "1" : "0") + "\n";
    }

    // nullopt when the line is not an instantiated line.
    inline std::optional<bool> decodeInstantiatedLine (const juce::String& line)
    {
        const auto t = line.trim();
        if (t == kLinePrefix + "I 1") return true;
        if (t == kLinePrefix + "I 0") return false;
        return std::nullopt;
    }

    // Collects the bytes a worker writes and hands out complete lines (without the line ending).
    class LineBuffer
    {
    public:
        std::vector<juce::String> add (const char* data, size_t size)
        {
            pending.append (data, size);
            std::vector<juce::String> lines;

            for (;;)
            {
                const auto* bytes = static_cast<const char*> (pending.getData());
                size_t end = 0;
                while (end < pending.getSize() && bytes[end] != '\n')
                    ++end;

                if (end == pending.getSize())
                    break;

                lines.push_back (juce::String::fromUTF8 (bytes, (int) end).trimEnd());
                pending.removeSection (0, end + 1);
            }
            return lines;
        }

    private:
        juce::MemoryBlock pending;
    };

    //==========================================================================
    // What happened while waiting for one reply.
    enum class Event { reply, workerGone, timeout };

    // What to do with the file.
    enum class Verdict
    {
        accept,                // keep the descriptions
        blacklist,             // the worker died (or never answered): blacklist exactly this file, no descriptions
        acceptWithoutBlacklist // keep the descriptions, only log (a test load that never finished, e.g. a modal dialog)
    };

    // Waiting for line 1: anything but a reply means the plugin file could not even be listed.
    inline Verdict verdictForListPhase (Event e)
    {
        return e == Event::reply ? Verdict::accept : Verdict::blacklist;
    }

    // Waiting for line 2, with the descriptions of line 1 in hand. A crash during the test load blacklists;
    // a timeout there does not (the plugin may be waiting on a dialog); a plugin that merely fails to instantiate
    // ("I 0") is kept, because nothing crashed.
    inline Verdict verdictForInstantiatePhase (Event e)
    {
        switch (e)
        {
            case Event::reply:      return Verdict::accept;
            case Event::workerGone: return Verdict::blacklist;
            case Event::timeout:    return Verdict::acceptWithoutBlacklist;
        }
        return Verdict::blacklist;
    }

    //==========================================================================
    // Fault injection requested through the environment (empty = none).
    inline juce::String testModeFromEnvironment()
    {
        return juce::SystemStats::getEnvironmentVariable (kTestEnvVar, {}).trim();
    }
}
