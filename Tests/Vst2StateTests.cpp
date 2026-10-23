//
//  Vst2StateTests.cpp — headless tests for the VST2 fxb/fxp parser, the VST3 state wrapper
//  and the replacement-name normalisation (Source/Engine/Vst2State.h).
//  Exposes runVst2StateTests(int& checks): appends to the check count, returns failures.
//

#include "../Source/Engine/Vst2State.h"
#include "../Source/Engine/PluginChoice.h"
#include <iostream>

using namespace juce;
namespace v2 = lighthost::vst2;

namespace
{
    int failures = 0;
    void check (int& checks, bool cond, const String& msg)
    {
        ++checks;
        if (! cond) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; }
    }

    using Bytes = std::vector<uint8_t>;

    void put32 (Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back ((uint8_t) (v >> s)); }
    void putTag (Bytes& b, const char* t) { for (int i = 0; i < 4; ++i) b.push_back ((uint8_t) t[i]); }
    void putF (Bytes& b, float f) { uint32_t u; std::memcpy (&u, &f, 4); put32 (b, u); }

    void putProgramHeader (Bytes& b, const char* magic, uint32_t n, const char* name)
    {
        putTag (b, "CcnK"); put32 (b, 0); putTag (b, magic);
        put32 (b, 1); put32 (b, 0x41424344); put32 (b, 7); put32 (b, n);
        for (int i = 0; i < 28; ++i)
            b.push_back (i < 27 && i < (int) std::strlen (name) ? (uint8_t) name[i] : (uint8_t) 0);
    }

    Bytes makeFxCk (const std::vector<float>& params, const char* name)
    {
        Bytes b; putProgramHeader (b, "FxCk", (uint32_t) params.size(), name);
        for (float f : params) putF (b, f);
        return b;
    }

    Bytes makeFPCh (const Bytes& chunk)
    {
        Bytes b; putProgramHeader (b, "FPCh", 0, "prog");
        put32 (b, (uint32_t) chunk.size());
        b.insert (b.end(), chunk.begin(), chunk.end());
        return b;
    }

    void putBankHeader (Bytes& b, const char* magic, uint32_t version, uint32_t numPrograms, uint32_t current)
    {
        putTag (b, "CcnK"); put32 (b, 0); putTag (b, magic);
        put32 (b, version); put32 (b, 0x41424344); put32 (b, 7); put32 (b, numPrograms);
        if (version >= 2) put32 (b, current); else put32 (b, 0);
        while (b.size() < 156) b.push_back (0);
    }

    Bytes makeFBCh (const Bytes& chunk, uint32_t version = 2)
    {
        Bytes b; putBankHeader (b, "FBCh", version, 1, 0);
        put32 (b, (uint32_t) chunk.size());
        b.insert (b.end(), chunk.begin(), chunk.end());
        return b;
    }

    Bytes makeFxBk (const std::vector<std::vector<float>>& programs, uint32_t version, uint32_t current)
    {
        Bytes b; putBankHeader (b, "FxBk", version, (uint32_t) programs.size(), current);
        int i = 0;
        for (const auto& p : programs)
        {
            const auto prog = makeFxCk (p, ("prog" + String (i++)).toRawUTF8());
            b.insert (b.end(), prog.begin(), prog.end());
        }
        return b;
    }

    // Every strict prefix of a valid blob must be rejected without reading out of bounds
    // (a prefix of a chunk bank can never be valid: its declared size no longer fits).
    void checkTruncation (int& checks, const Bytes& full, const String& label)
    {
        bool allRejected = true;
        for (size_t n = 0; n < full.size(); ++n)
            if (v2::parse (full.data(), n).has_value())
                allRejected = false;
        check (checks, allRejected, label + ": every truncation is rejected");
    }
}

int runVst2StateTests (int& checks)
{
    failures = 0;
    std::cout << "VST2 state tests" << std::endl;

    // --- 1. FxCk / FPCh / FBCh / FxBk parse ------------------------------------
    {
        const auto fxck = makeFxCk ({ 0.25f, 0.5f, 1.0f }, "Lead");
        const auto b = v2::parse (fxck.data(), fxck.size());
        check (checks, b.has_value() && b->kind == v2::Kind::programParams, "FxCk is programParams");
        check (checks, b.has_value() && b->params.size() == 3 && b->params[1] == 0.5f, "FxCk params read");
        check (checks, b.has_value() && b->programName == "Lead", "FxCk program name read");
        check (checks, b.has_value() && b->fxID == 0x41424344 && b->fxVersion == 7, "FxCk fxID / fxVersion read");
        checkTruncation (checks, fxck, "FxCk");
    }
    {
        const Bytes chunk { 1, 2, 3, 4, 5 };
        const auto fpch = makeFPCh (chunk);
        const auto b = v2::parse (fpch.data(), fpch.size());
        check (checks, b.has_value() && b->kind == v2::Kind::programChunk, "FPCh is programChunk");
        check (checks, b.has_value() && b->chunk.getSize() == 5 && ((const uint8_t*) b->chunk.getData())[4] == 5, "FPCh chunk read");
        checkTruncation (checks, fpch, "FPCh");

        const auto fbch = makeFBCh (chunk);
        const auto c = v2::parse (fbch.data(), fbch.size());
        check (checks, c.has_value() && c->kind == v2::Kind::bankChunk, "FBCh is bankChunk");
        check (checks, c.has_value() && c->chunk.getSize() == 5, "FBCh chunk read");
        checkTruncation (checks, fbch, "FBCh");
    }
    {
        const auto v2bank = makeFxBk ({ { 0.1f }, { 0.2f, 0.3f } }, 2, 1);
        const auto b = v2::parse (v2bank.data(), v2bank.size());
        check (checks, b.has_value() && b->kind == v2::Kind::bankParams, "FxBk is bankParams");
        check (checks, b.has_value() && b->currentProgram == 1, "v2 bank: currentProgram read");
        check (checks, b.has_value() && b->params.size() == 2 && b->params[1] == 0.3f, "v2 bank: program 1 params picked");
        check (checks, b.has_value() && b->programName == "prog1", "v2 bank: program 1 name picked");
        checkTruncation (checks, v2bank, "FxBk v2");

        // Version 1 has no currentProgram: the field is ignored, program 0 is used.
        auto v1bank = makeFxBk ({ { 0.1f }, { 0.2f, 0.3f } }, 1, 0);
        const auto c = v2::parse (v1bank.data(), v1bank.size());
        check (checks, c.has_value() && c->currentProgram == 0 && c->params.size() == 1, "v1 bank: program 0");

        // Out-of-range currentProgram clamps to 0.
        const auto oob = makeFxBk ({ { 0.1f }, { 0.2f } }, 2, 9);
        const auto d = v2::parse (oob.data(), oob.size());
        check (checks, d.has_value() && d->currentProgram == 0 && d->params[0] == 0.1f, "currentProgram out of range clamps to 0");
    }

    // --- 2. Malformed input ------------------------------------------------------
    {
        auto bad = makeFxCk ({ 0.5f }, "x");
        bad[0] = 'X';
        check (checks, ! v2::parse (bad.data(), bad.size()).has_value(), "wrong CcnK magic rejected");

        auto badKind = makeFxCk ({ 0.5f }, "x");
        badKind[8] = 'Z';
        check (checks, ! v2::parse (badKind.data(), badKind.size()).has_value(), "unknown fxMagic rejected");

        auto over = makeFBCh (Bytes { 1, 2, 3 });
        over[156] = 0x7f;   // declared chunk size far beyond the buffer
        check (checks, ! v2::parse (over.data(), over.size()).has_value(), "oversize chunkSize rejected");

        auto manyPrograms = makeFxBk ({ { 0.1f } }, 2, 0);
        manyPrograms[24] = 0xff; manyPrograms[25] = 0xff;   // numPrograms * program size overflows the buffer
        check (checks, ! v2::parse (manyPrograms.data(), manyPrograms.size()).has_value(), "numPrograms beyond the buffer rejected");

        auto hugeParams = makeFxCk ({ 0.5f }, "x");
        hugeParams[24] = 0xff; hugeParams[25] = 0xff; hugeParams[26] = 0xff; hugeParams[27] = 0xff;   // 32-bit overflow bait
        check (checks, ! v2::parse (hugeParams.data(), hugeParams.size()).has_value(), "huge numParams rejected");

        check (checks, ! v2::parse (nullptr, 0).has_value(), "null data rejected");
        const uint8_t junk[] = { 'C', 'c', 'n', 'K' };
        check (checks, ! v2::parse (junk, sizeof (junk)).has_value(), "magic-only buffer rejected");
    }

    // --- 3. VST3 state wrapper round trip ------------------------------------------
    {
        const Bytes payload { 9, 8, 7, 0, 255, 1 };
        const auto wrapped = v2::wrapAsVst3State (payload.data(), payload.size());
        const auto xml = AudioProcessor::getXmlFromBinary (wrapped.getData(), (int) wrapped.getSize());
        check (checks, xml != nullptr && xml->hasTagName ("VST3PluginState"), "wrapper root is VST3PluginState");
        auto* comp = xml != nullptr ? xml->getChildByName ("IComponent") : nullptr;
        check (checks, comp != nullptr, "wrapper has an IComponent child");
        MemoryBlock back;
        check (checks, comp != nullptr && back.fromBase64Encoding (comp->getAllSubText()), "IComponent text is base64");
        check (checks, back.getSize() == payload.size() && std::memcmp (back.getData(), payload.data(), payload.size()) == 0,
               "wrapper round trip returns the original bytes");
    }

    // --- 4. normaliseName ----------------------------------------------------------
    {
        using v2::normaliseName;
        check (checks, normaliseName ("Auburn Sounds Graillon 2-64", "Auburn Sounds") == normaliseName ("Graillon 2", "Auburn Sounds"),
               "Graillon 2-64 matches Graillon 2");
        check (checks, normaliseName ("LoudMax64") == normaliseName ("LoudMax"), "LoudMax64 matches LoudMax");
        check (checks, normaliseName ("LoudMax64") == "loudmax", "LoudMax64 normalises to loudmax (x64 not eaten off 'max')");
        check (checks, normaliseName ("BC Triple EQ 4 VST(Stereo)") == normaliseName ("Triple EQ 4"),
               "BC Triple EQ 4 VST(Stereo) matches Triple EQ 4");
        check (checks, normaliseName ("BC Triple EQ 4 VST(Stereo)") == normaliseName ("BC Triple EQ 4 VST3 (Stereo)"),
               "Blue Cat VST and VST3 names match");
        check (checks, normaliseName ("Marvel GEQ") == normaliseName ("Marvel GEQ"), "Marvel GEQ matches itself");
        check (checks, normaliseName ("Marvel GEQ 64") == normaliseName ("Marvel GEQ"), "trailing 64 dropped");
        check (checks, normaliseName ("Reverb 264") == "reverb264", "264 keeps its digits");
        check (checks, normaliseName ("TAL-Dub-3") == normaliseName ("TAL Dub 3"), "punctuation ignored");
        check (checks, normaliseName ("Triple EQ 4") != normaliseName ("Triple EQ 3"), "different plugins stay different");
        check (checks, normaliseName ("") == "", "empty name stays empty");
    }

    // --- 5. Menu helpers: unloadable formats hidden, replacements suggested ----------
    {
        const auto make = [] (const String& name, const String& maker, const String& format, const String& file)
        {
            PluginDescription d;
            d.name = name; d.manufacturerName = maker; d.pluginFormatName = format; d.fileOrIdentifier = file;
            return d;
        };

        Array<PluginDescription> known;
        known.add (make ("Graillon 2-64", "Auburn Sounds", "VST", "C:/old/Graillon.dll"));       // 0 stale VST2
        known.add (make ("Graillon 2", "Auburn Sounds", "VST3", "C:/vst3/Graillon 2.vst3"));    // 1
        known.add (make ("Bitcrush", "Auburn Sounds", "VST3", "C:/vst3/Bitcrush.vst3"));        // 2 same maker
        known.add (make ("LoudMax", "Thomas Mundt", "VST3", "C:/vst3/LoudMax.vst3"));           // 3
        known.add (make ("Other", "Someone", "VST3", "C:/vst3/Other.vst3"));                    // 4

        const auto loadable = lighthost::choice::filterLoadable (known, StringArray { "VST3" });
        check (checks, loadable.size() == 4, "filterLoadable drops entries whose format isn't registered");
        check (checks, lighthost::choice::filterLoadable (known, StringArray { "VST3", "VST" }).size() == 5,
               "filterLoadable keeps a format once it is registered");
        check (checks, known.size() == 5, "filterLoadable leaves the stored list alone");

        const auto sug = lighthost::choice::suggestReplacements (known[0], loadable);
        check (checks, sug.size() == 2 && loadable[sug[0]].name == "Graillon 2" && loadable[sug[1]].name == "Bitcrush",
               "suggestions: same normalised name first, same manufacturer after");

        const auto none = lighthost::choice::suggestReplacements (make ("Unknown", "Nobody", "VST", "x.dll"), loadable);
        check (checks, none.empty(), "no suggestion for an unrelated plugin");

        const auto loud = lighthost::choice::suggestReplacements (make ("LoudMax64", "Thomas Mundt", "VST", "LoudMax64.dll"), loadable);
        check (checks, loud.size() == 1 && loadable[loud[0]].name == "LoudMax", "LoudMax64 suggests LoudMax");
    }

    std::cout << "  VST2 state: " << (failures == 0 ? "ok" : "FAILURES") << std::endl;
    return failures;
}
