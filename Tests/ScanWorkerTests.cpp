//
//  ScanWorkerTests.cpp — headless tests for the pure part of out-of-process plugin scanning (ScanProtocol.h):
//  the worker's command line, its reply lines and the decision table. The worker processes themselves are
//  exercised by Test X in SelfTest.cpp. Exposes runScanWorkerTests(int& checks): appends to the check count,
//  returns failures.
//

#include "../Source/Engine/ScanProtocol.h"
#include <iostream>

using namespace juce;
namespace sc = lighthost::scan;

namespace
{
    int failures = 0;
    void check (int& checks, bool cond, const String& msg)
    {
        ++checks;
        if (! cond) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; }
    }

    PluginDescription describe (const String& name, bool instrument)
    {
        PluginDescription d;
        d.name = name;
        d.pluginFormatName = "VST3";
        d.fileOrIdentifier = "C:\\VST3\\" + name + ".vst3";
        d.manufacturerName = "Acme";
        d.isInstrument = instrument;
        d.numInputChannels = instrument ? 0 : 2;
        d.numOutputChannels = 2;
        d.uniqueId = d.deprecatedUid = 1234;
        return d;
    }
}

int runScanWorkerTests (int& checks)
{
    failures = 0;
    std::cout << "Scan worker protocol tests" << std::endl;

    // The worker's command line and its parameters.
    {
        const File exe ("C:\\Program Files\\LightHost\\LightHost GUI.exe");
        const auto args = sc::workerCommandLine (exe, { "VST3", "C:\\VST3\\Some Plugin.vst3" });
        check (checks, args.size() == 4 && args[1] == "--lighthostscan" && args[2] == "VST3" && args[3] == "C:\\VST3\\Some Plugin.vst3",
               "command line: flag, format and file follow the exe");

        StringArray params;
        for (int i = 1; i < args.size(); ++i)
            params.add (args[i]);
        const auto back = sc::parseWorkerParameters (params);
        check (checks, back.has_value() && back->formatName == "VST3" && back->fileOrIdentifier == "C:\\VST3\\Some Plugin.vst3",
               "parameters: format and file (with a space in the path) survive");
        check (checks, ! sc::parseWorkerParameters ({}).has_value(), "parameters: none is refused");
        check (checks, ! sc::parseWorkerParameters ({ "--lighthostscan", "VST3" }).has_value(), "parameters: a missing file is refused");
        check (checks, ! sc::parseWorkerParameters ({ "--lighthostscan", "", "x" }).has_value(), "parameters: an empty format is refused");
        check (checks, ! sc::parseWorkerParameters ({ "--other", "VST3", "x" }).has_value(), "parameters: another flag is refused");
        check (checks, ! sc::parseWorkerParameters ({ "--lighthostscan", "VST3", "x", "y" }).has_value(), "parameters: extra parameters are refused");

        check (checks, sc::isWorkerCommandLine ("--lighthostscan VST3 \"C:\\a b.vst3\""), "isWorkerCommandLine: a worker's command line");
        check (checks, sc::isWorkerCommandLine ("  --lighthostscan"), "isWorkerCommandLine: leading blanks, flag only");
        check (checks, ! sc::isWorkerCommandLine ("--lighthostscanner x"), "isWorkerCommandLine: a longer flag is not one");
        check (checks, ! sc::isWorkerCommandLine (""), "isWorkerCommandLine: the normal start is not one");
        check (checks, ! sc::isWorkerCommandLine ("-run-selftest"), "isWorkerCommandLine: -run-selftest is not one");
    }

    // Line 1: descriptions round trip, several entries, an empty list is still a list, one line per message.
    {
        OwnedArray<PluginDescription> found;
        found.add (new PluginDescription (describe ("Alpha", false)));
        found.add (new PluginDescription (describe ("Beta\nWith Newline", true)));

        const auto line = sc::encodeListLine (found);
        check (checks, line.endsWith ("\n") && line.trimEnd().containsChar ('\n') == false, "list line: exactly one line, even with a newline in a name");

        const auto back = sc::decodeListLine (line.trimEnd());
        check (checks, back.has_value() && back->size() == 2, "list line: two descriptions come back");
        check (checks, back.has_value() && back->size() == 2 && (*back)[0].name == "Alpha" && ! (*back)[0].isInstrument
                       && (*back)[0].fileOrIdentifier == found[0]->fileOrIdentifier && (*back)[0].numInputChannels == 2,
               "list line: the first description keeps its fields");
        check (checks, back.has_value() && back->size() == 2 && (*back)[1].name == "Beta\nWith Newline" && (*back)[1].isInstrument
                       && (*back)[1].numInputChannels == 0,
               "list line: the instrument flag and a newline in the name survive");

        const OwnedArray<PluginDescription> none;
        const auto empty = sc::decodeListLine (sc::encodeListLine (none).trimEnd());
        check (checks, empty.has_value() && empty->empty(), "list line: an empty list decodes as empty, not as a failure");
        check (checks, ! sc::decodeListLine ("L not xml").has_value(), "list line: garbage is refused");
        check (checks, ! sc::decodeListLine ("<LIST/>").has_value(), "list line: no prefix is refused");
        check (checks, ! sc::decodeListLine (sc::encodeInstantiatedLine (true)).has_value(), "list line: another line is refused");
    }

    // Line 2.
    {
        check (checks, sc::decodeInstantiatedLine (sc::encodeInstantiatedLine (true)) == std::optional<bool> (true), "instantiated line: ok = 1");
        check (checks, sc::decodeInstantiatedLine (sc::encodeInstantiatedLine (false)) == std::optional<bool> (false), "instantiated line: ok = 0");
        check (checks, ! sc::decodeInstantiatedLine ("L <LIST/>").has_value(), "instantiated line: another line is refused");
        check (checks, ! sc::decodeInstantiatedLine ("").has_value(), "instantiated line: an empty text is refused");
    }

    // The byte stream of stdout becomes lines, however it is chunked.
    {
        const String stream = sc::encodeListLine (OwnedArray<PluginDescription>()) + sc::encodeInstantiatedLine (true);
        const auto utf8 = stream.toStdString();

        sc::LineBuffer whole;
        const auto all = whole.add (utf8.data(), utf8.size());
        check (checks, all.size() == 2 && all[0] == sc::kLinePrefix + "L <LIST/>" && all[1] == sc::kLinePrefix + "I 1",
               "line buffer: one chunk gives both lines");

        sc::LineBuffer bytes;
        std::vector<String> got;
        for (char c : utf8)
            for (const auto& l : bytes.add (&c, 1))
                got.push_back (l);
        check (checks, got == all, "line buffer: one byte at a time gives the same lines");

        sc::LineBuffer partial;
        check (checks, partial.add ("L <LI", 5).empty(), "line buffer: an incomplete line is held back");
        const auto rest = partial.add ("ST/>\r\n", 6);
        check (checks, rest.size() == 1 && rest[0] == "L <LIST/>", "line buffer: the line completes, CRLF is trimmed");
    }

    // C-R1: every protocol line carries the prefix; anything else on the pipe is a plugin's noise.
    {
        OwnedArray<PluginDescription> found;
        found.add (new PluginDescription (describe ("Alpha", false)));
        const auto list = sc::encodeListLine (found).trimEnd();
        const auto done = sc::encodeInstantiatedLine (true).trimEnd();

        check (checks, sc::isProtocolLine (list) && sc::isProtocolLine (done), "prefix: both worker lines are protocol lines");
        check (checks, list.startsWith ("@LHSCAN1 L ") && done == "@LHSCAN1 I 1", "prefix: wording of the two lines");
        check (checks, ! sc::isProtocolLine ("plugin banner") && ! sc::isProtocolLine ("L <LIST/>") && ! sc::isProtocolLine ("")
                       && ! sc::isProtocolLine ("@LHSCAN2 L <LIST/>"),
               "prefix: noise, an unprefixed line and another version are not protocol lines");
        check (checks, ! sc::decodeListLine ("L <LIST/>").has_value() && ! sc::decodeInstantiatedLine ("I 1").has_value(),
               "prefix: the old unprefixed lines are refused");

        // A byte stream with noise around the real lines: only the protocol lines survive the filter.
        const String stream = String ("banner\r\n") + list + "\nwarning: something\n" + done + "\n";
        sc::LineBuffer buffer;
        std::vector<String> kept;
        const auto utf8 = stream.toStdString();
        for (const auto& l : buffer.add (utf8.data(), utf8.size()))
            if (sc::isProtocolLine (l))
                kept.push_back (l);
        check (checks, kept.size() == 2 && sc::decodeListLine (kept[0]).has_value() && sc::decodeInstantiatedLine (kept[1]) == std::optional<bool> (true),
               "prefix: noise lines are skipped, the real ones decode");
    }

    // The decision table: what each way of ending means for the file.
    {
        using E = sc::Event;
        using V = sc::Verdict;

        check (checks, sc::verdictForListPhase (E::reply) == V::accept, "verdict, before line 1: a reply is accepted");
        check (checks, sc::verdictForListPhase (E::workerGone) == V::blacklist, "verdict, before line 1: a crash blacklists");
        check (checks, sc::verdictForListPhase (E::timeout) == V::blacklist, "verdict, before line 1: a timeout blacklists");

        check (checks, sc::verdictForInstantiatePhase (E::reply) == V::accept, "verdict, test load: a reply is accepted");
        check (checks, sc::verdictForInstantiatePhase (E::workerGone) == V::blacklist, "verdict, test load: a crash blacklists");
        check (checks, sc::verdictForInstantiatePhase (E::timeout) == V::acceptWithoutBlacklist,
               "verdict, test load: a timeout keeps the descriptions and does not blacklist");
    }

    // Fault injection is off unless the environment asks for it.
    {
        check (checks, sc::testModeFromEnvironment().isEmpty(), "test mode: off by default");
    }

    return failures;
}
