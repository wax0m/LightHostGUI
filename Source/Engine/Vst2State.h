//
//  Vst2State.h — pure helpers for importing a saved VST2 state into a VST3 instance.
//  Header-only and headless-testable: a bounds-checked fxb/fxp parser, a wrapper that
//  turns raw bytes into the XML JUCE's VST3 host feeds to IComponent::setState, and the
//  name normalisation behind the "Suggested" replacement list.
//

#pragma once

#include <JuceHeader.h>
#include <optional>
#include <vector>

namespace lighthost::vst2
{
    enum class Kind { bankChunk, programChunk, bankParams, programParams };   // FBCh, FPCh, FxBk, FxCk

    struct Blob
    {
        Kind kind = Kind::bankParams;
        int32_t fxID = 0, fxVersion = 0, currentProgram = 0;
        juce::MemoryBlock chunk;        // FBCh / FPCh only
        std::vector<float> params;      // current program's params (FxBk: program[currentProgram], FxCk: the program)
        juce::String programName;
    };

    namespace detail
    {
        constexpr size_t bankHeaderSize    = 156;   // chunkMagic .. numPrograms, currentProgram, reserved
        constexpr size_t programHeaderSize = 56;    // chunkMagic .. numParams, 28-byte name

        struct Reader
        {
            const uint8_t* p; size_t size;

            bool has (size_t offset, size_t n) const noexcept { return offset <= size && n <= size - offset; }

            bool u32 (size_t offset, uint32_t& out) const noexcept
            {
                if (! has (offset, 4)) return false;
                out = ((uint32_t) p[offset] << 24) | ((uint32_t) p[offset + 1] << 16)
                    | ((uint32_t) p[offset + 2] << 8) | (uint32_t) p[offset + 3];
                return true;
            }

            bool tag (size_t offset, const char* t) const noexcept
            {
                return has (offset, 4) && std::memcmp (p + offset, t, 4) == 0;
            }

            bool f32 (size_t offset, float& out) const noexcept
            {
                uint32_t bits = 0;
                if (! u32 (offset, bits)) return false;
                std::memcpy (&out, &bits, 4);
                return true;
            }

            juce::String name (size_t offset) const
            {
                char buf[29] = {};
                if (has (offset, 28))
                    std::memcpy (buf, p + offset, 28);
                return juce::String::fromUTF8 (buf);
            }
        };

        // Parses the 56-byte program header at `at`. On success `numParamsOrPrograms` is the
        // field at +24 (numParams for FxCk, numPrograms slot for FPCh).
        inline bool readProgramHeader (const Reader& r, size_t at, const char* magic, Blob& b, uint32_t& n)
        {
            uint32_t id = 0, ver = 0;
            if (! r.tag (at, "CcnK") || ! r.tag (at + 8, magic)
                || ! r.u32 (at + 16, id) || ! r.u32 (at + 20, ver) || ! r.u32 (at + 24, n)
                || ! r.has (at, programHeaderSize))
                return false;
            b.fxID = (int32_t) id;
            b.fxVersion = (int32_t) ver;
            b.programName = r.name (at + 28);
            return true;
        }

        inline bool readFloats (const Reader& r, size_t at, uint32_t n, std::vector<float>& out)
        {
            if (n > (r.size - juce::jmin (at, r.size)) / 4)   // also guards n * 4 overflow
                return false;
            out.resize (n);
            for (uint32_t i = 0; i < n; ++i)
                if (! r.f32 (at + (size_t) i * 4, out[i]))
                    return false;
            return true;
        }

        inline bool readChunk (const Reader& r, size_t sizeAt, juce::MemoryBlock& out)
        {
            uint32_t n = 0;
            if (! r.u32 (sizeAt, n) || ! r.has (sizeAt + 4, n))
                return false;
            out.replaceAll (r.p + sizeAt + 4, n);
            return true;
        }
    }

    // nullopt on anything malformed; never reads out of bounds.
    inline std::optional<Blob> parse (const void* data, size_t size)
    {
        if (data == nullptr)
            return std::nullopt;

        const detail::Reader r { static_cast<const uint8_t*> (data), size };
        if (! r.tag (0, "CcnK"))
            return std::nullopt;

        Blob b;
        uint32_t n = 0;

        if (r.tag (8, "FxCk"))
        {
            b.kind = Kind::programParams;
            if (! detail::readProgramHeader (r, 0, "FxCk", b, n)
                || ! detail::readFloats (r, detail::programHeaderSize, n, b.params))
                return std::nullopt;
            return b;
        }

        if (r.tag (8, "FPCh"))
        {
            b.kind = Kind::programChunk;
            if (! detail::readProgramHeader (r, 0, "FPCh", b, n)
                || ! detail::readChunk (r, detail::programHeaderSize, b.chunk))
                return std::nullopt;
            return b;
        }

        const bool isChunkBank  = r.tag (8, "FBCh");
        const bool isParamsBank = r.tag (8, "FxBk");
        if (! isChunkBank && ! isParamsBank)
            return std::nullopt;

        uint32_t version = 0, id = 0, ver = 0, numPrograms = 0;
        if (! r.has (0, detail::bankHeaderSize)
            || ! r.u32 (12, version) || ! r.u32 (16, id) || ! r.u32 (20, ver) || ! r.u32 (24, numPrograms))
            return std::nullopt;

        b.fxID = (int32_t) id;
        b.fxVersion = (int32_t) ver;

        if (version >= 2)   // currentProgram exists only in the version-2 header
        {
            uint32_t cur = 0;
            if (! r.u32 (28, cur))
                return std::nullopt;
            b.currentProgram = (cur < numPrograms) ? (int32_t) cur : 0;
        }

        if (isChunkBank)
        {
            b.kind = Kind::bankChunk;
            if (! detail::readChunk (r, detail::bankHeaderSize, b.chunk))
                return std::nullopt;
            return b;
        }

        b.kind = Kind::bankParams;
        if (numPrograms == 0)
            return std::nullopt;

        size_t at = detail::bankHeaderSize;
        for (uint32_t i = 0; i < numPrograms; ++i)
        {
            Blob prog;
            uint32_t numParams = 0;
            if (! detail::readProgramHeader (r, at, "FxCk", prog, numParams)
                || numParams > (size - at - detail::programHeaderSize) / 4)
                return std::nullopt;

            if ((int32_t) i == b.currentProgram)
            {
                if (! detail::readFloats (r, at + detail::programHeaderSize, numParams, b.params))
                    return std::nullopt;
                b.programName = prog.programName;
            }
            at += detail::programHeaderSize + (size_t) numParams * 4;
        }
        return b;
    }

    // XmlElement "VST3PluginState" + IComponent, as VST3PluginInstance::setStateInformation parses it.
    inline juce::MemoryBlock wrapAsVst3State (const void* componentState, size_t size)
    {
        juce::MemoryBlock component (componentState, size);

        juce::XmlElement state ("VST3PluginState");
        state.createNewChildElement ("IComponent")->addTextElement (component.toBase64Encoding());

        juce::MemoryBlock out;
        juce::AudioProcessor::copyXmlToBinary (state, out);
        return out;
    }

    // Reduces a plugin name to a comparable key: lowercase, without the manufacturer prefix,
    // format/bitness decoration and non-alphanumerics. `manufacturer` is optional.
    inline juce::String normaliseName (const juce::String& name, const juce::String& manufacturer = {})
    {
        auto s = name.toLowerCase().trim();

        const auto maker = manufacturer.toLowerCase().trim();
        if (maker.isNotEmpty() && s.startsWith (maker))
            s = s.substring (maker.length()).trim();

        for (const char* junk : { "(stereo)", "(dual)", "vst3", "vst" })
            s = s.replace (junk, " ");
        s = s.trim();

        for (const char* suffix : { "x64", "64" })   // "LoudMax64", "Graillon 2-64"; never "Reverb 264"
        {
            const juce::String sfx (suffix);
            const auto before = s.length() > sfx.length() ? s[s.length() - sfx.length() - 1] : (juce::juce_wchar) 0;
            const bool boundary = before != 0 && ! juce::CharacterFunctions::isDigit (before)
                                  && (sfx.length() == 2 || ! juce::CharacterFunctions::isLetter (before));   // "x64" only as its own word: "LoudMax64" is "max" + "64"
            if (s.endsWith (sfx) && boundary)
            {
                s = s.dropLastCharacters (sfx.length()).trim();
                break;
            }
        }

        if (s.startsWith ("bc "))
            s = s.substring (3);

        return s.retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789");
    }
}
