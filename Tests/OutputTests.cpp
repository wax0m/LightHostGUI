//
//  OutputTests.cpp — headless tests for output nodes bound to device channel pairs: the pure rules in
//  OutputPairs.h, the document side (bindings, main OUT, legacy pinning) and the preset store.
//  Exposes runOutputTests(int& checks): appends to the check count, returns failures.
//

#include "../Source/Engine/OutputPairs.h"
#include "../Source/Engine/GraphDocument.h"
#include "../Source/Engine/PresetStore.h"
#include <iostream>

using namespace juce;
namespace op = lighthost::outputs;

namespace
{
    int failures = 0;
    void check (int& checks, bool cond, const String& msg)
    {
        ++checks;
        if (! cond) { ++failures; std::cerr << "  FAIL: " << msg << std::endl; }
    }

    BigInteger mask (const char* bits)
    {
        BigInteger b;
        b.parseString (bits, 2);   // "1100" = channels 3 and 4 (bit 0 is the rightmost digit)
        return b;
    }

    op::Layout layoutOf (const String& device, const char* bits)
    {
        op::Layout l;
        l.known = true;
        l.deviceName = device;
        l.activeMask = mask (bits);
        l.names = StringArray { "Out 1", "Out 2", "Loop-back 1", "Loop-back 2" };
        return l;
    }
}

int runOutputTests (int& checks)
{
    failures = 0;
    std::cout << "Output pair tests" << std::endl;

    // --- 1. Pure rules ------------------------------------------------------------
    {
        check (checks, op::firstActiveOutput (mask ("1100")) == 2, "Hung's mask 1100: first active output is channel index 2");
        check (checks, op::firstActiveOutput (mask ("0110")) == 1 && op::firstActiveOutput (mask ("0")) == -1, "first active: 0110 -> 1, empty -> -1");

        check (checks, op::activeIndex (mask ("1111"), 2) == 2 && op::activeIndex (mask ("1111"), 0) == 0, "mask 1111: pair 2 lands on graph channel 2");
        check (checks, op::activeIndex (mask ("1100"), 2) == 0, "mask 1100: pair 2 is the first active pair (graph channel 0)");
        check (checks, op::activeIndex (mask ("1100"), 0) == -1, "mask 1100: pair 0 is inactive");
        check (checks, op::activeIndex (mask ("1101"), 0) == -1 && op::activeIndex (mask ("1010"), 1) == -1, "a half-active pair is inactive");
        check (checks, op::activeIndex (mask ("0011"), 2) == -1 && op::activeIndex (mask ("1111"), -1) == -1, "inactive / invalid channels give -1");

        check (checks, op::pairLabel (StringArray { "Out 1", "Out 2", "Loop-back 1", "Loop-back 2" }, 2) == "Loop-back 1/2", "label: Loop-back 1/2");
        check (checks, op::pairLabel (StringArray { "Out 1", "Out 2" }, 0) == "Out 1/2", "label: Out 1/2");
        check (checks, op::pairLabel (StringArray { "Line L", "Line R" }, 0) == "Line L/R", "label: Line L/R");
        check (checks, op::pairLabel (StringArray {}, 2) == "Out 3/4", "label falls back to Out 3/4 without names");
        check (checks, op::pairLabel (StringArray { "Mic", "Guitar" }, 0) == "Mic + Guitar", "label joins unrelated names");

        const auto pin = op::pinTarget (layoutOf ("EVO", "1100"));
        check (checks, pin.has_value() && pin->outCh == 2 && pin->label == "Loop-back 1/2" && pin->device == "EVO",
               "pin target: first active pair, its label, the device");
        check (checks, ! op::pinTarget (layoutOf ("EVO", "0")).has_value(), "pin target: no active output -> none");
        check (checks, ! op::pinTarget (op::Layout()).has_value(), "pin target: no device -> none");

        // outputBase: strict on the same device, fallback / silence elsewhere.
        const auto evo = layoutOf ("EVO", "1111"), other = layoutOf ("Other", "1111");
        op::Binding mainB; mainB.isMain = true; mainB.bound = true; mainB.outCh = 2; mainB.outDevice = "EVO";
        op::Binding extraB = mainB; extraB.isMain = false;
        check (checks, op::outputBase (mainB, evo) == 2 && op::outputBase (extraB, evo) == 2, "same device: strict binding (pair 2 -> graph channel 2)");
        check (checks, op::outputBase (mainB, other) == 0, "other device: the main OUT falls back to the first active pair");
        check (checks, op::outputBase (extraB, other) == -1, "other device: an extra OUT is inactive");
        check (checks, op::outputBase (mainB, layoutOf ("EVO", "0011")) == -1, "same device, bound pair not active: silent");
        op::Binding unbound; unbound.isMain = true;
        check (checks, op::outputBase (unbound, evo) == 0, "unbound main OUT: first active pair (pre-v1.2 behaviour)");
        check (checks, op::outputBase (mainB, op::Layout()) == 0 && op::outputBase (extraB, op::Layout()) == -1,
               "no device yet: main on the first pair, extra inactive");
        op::Binding anyDevice = mainB; anyDevice.outDevice = {};
        check (checks, op::outputBase (anyDevice, other) == 2, "a binding without a device name applies to any device");
    }

    // --- 2. Document: main OUT, bindings, legacy pinning --------------------------------
    {
        GraphDocument doc;
        const String main = doc.getIoNodeUid (false);
        check (checks, doc.isMainOutput (main) && doc.getOutputUids().size() == 1, "a new document has one OUT and it is the main one");
        check (checks, doc.getOutputChannel (main) == -1, "the original OUT starts unbound");

        const String extra = doc.addOutputNode (0, "Out 1/2", "EVO", 100.0f, 500.0f);
        check (checks, doc.getOutputUids().size() == 2 && doc.getIoNodeUid (false) == main && ! doc.isMainOutput (extra),
               "an extra OUT never takes over as the main one");
        check (checks, doc.getOutputChannel (extra) == 0 && doc.getOutputLabel (extra) == "Out 1/2" && doc.getOutputDevice (extra) == "EVO",
               "an extra OUT carries its binding");
        doc.setOutputBinding (extra, 2, "Loop-back 1/2", "EVO");
        check (checks, doc.getOutputChannel (extra) == 2 && doc.getOutputLabel (extra) == "Loop-back 1/2", "rebinding replaces the binding");

        const auto rt = GraphDocument::fromXml (doc.toXml());
        check (checks, rt.getOutputChannel (extra) == 2 && rt.getOutputDevice (extra) == "EVO" && rt.isMainOutput (main) && rt.getOutputChannel (main) == -1,
               "outCh / outLabel / outDevice / main round-trip through XML");

        // Legacy pinning.
        check (checks,   doc.pinLegacyOutputs (2, "Loop-back 1/2", "EVO"), "pinning stamps the unbound main OUT");
        check (checks,   doc.getOutputChannel (main) == 2 && doc.getOutputLabel (main) == "Loop-back 1/2" && doc.getOutputDevice (main) == "EVO",
               "the main OUT is now bound to channel index 2 on EVO");
        check (checks, ! doc.pinLegacyOutputs (2, "Loop-back 1/2", "EVO"), "pinning is idempotent");
        check (checks, ! doc.pinLegacyOutputs (0, "Out 1/2", "Other"), "a bound OUT is never re-stamped, also not by another device");
        check (checks,   doc.getOutputChannel (main) == 2 && doc.getOutputDevice (main) == "EVO", "pinning on another device leaves the EVO binding alone");

        // Documents from before v1.2 have no main flag: the first OUT counts as main and gets flagged.
        GraphDocument old = GraphDocument::fromXml (doc.toXml());
        auto nodes = old.getNodes();
        for (int i = 0; i < nodes.getNumChildren(); ++i)
        {
            auto n = nodes.getChild (i);
            n.removeProperty ("main", nullptr);
            if (n.getProperty ("type").toString() == GraphDocument::typeAudioOut)
                n.removeProperty ("outCh", nullptr);
        }
        check (checks, old.getIoNodeUid (false) == main, "no main flag: the first audioOut is the main OUT");
        check (checks, old.pinLegacyOutputs (2, "Loop-back 1/2", "EVO") && old.isMainOutput (main) && old.getOutputChannel (extra) == 2,
               "pinning also flags the main OUT of an old document and binds every unbound OUT");
    }

    // --- 3. Chain rules with extra OUTs ----------------------------------------------------
    {
        PluginDescription synth; synth.name = "Piano"; synth.pluginFormatName = "VST3"; synth.isInstrument = true; synth.numOutputChannels = 2;
        PluginDescription effect; effect.name = "Fx"; effect.pluginFormatName = "VST3"; effect.numInputChannels = effect.numOutputChannels = 2;

        GraphDocument doc;
        const String in = doc.getIoNodeUid (true), main = doc.getIoNodeUid (false);
        const String extra = doc.addOutputNode (0, "Out 1/2", "EVO", 100.0f, 500.0f);
        const String fx = doc.addPluginNode (effect, 300.0f, 260.0f);
        const String piano = doc.addPluginNode (synth, 100.0f, 560.0f);
        for (int ch = 0; ch < 2; ++ch)
        {
            doc.addConnection (in, ch, fx, ch);
            doc.addConnection (fx, ch, main, ch);
            doc.addConnection (piano, ch, main, ch);
            doc.addConnection (piano, ch, extra, ch);
        }
        check (checks, doc.isLinearChain(), "mic chain + piano wired to main OUT and an extra OUT is still linear");
        check (checks, doc.canAddConnection (fx, 0, extra, 0), "an effect may also feed the extra OUT");
        doc.addConnection (fx, 0, extra, 0);
        check (checks, ! doc.isLinearChain(), "an effect feeding the extra OUT branches the chain");
    }

    // --- 4. Presets: pinning reaches every stored preset ------------------------------------
    {
        PresetStore store;
        GraphDocument a, b, c;
        b.setOutputBinding (b.getIoNodeUid (false), 0, "Out 1/2", "Other");   // a preset already bound on another device
        c.addOutputNode (0, "Out 1/2", "EVO", 100.0f, 500.0f);
        store.addPreset ("A", a);
        store.addPreset ("B", b);
        store.addPreset ("C", c);

        check (checks,   store.pinLegacyOutputs (2, "Loop-back 1/2", "EVO"), "preset pinning reports a change");
        const auto pa = store.getPresetDocument (0), pb = store.getPresetDocument (1), pc = store.getPresetDocument (2);
        check (checks, pa.getOutputChannel (pa.getIoNodeUid (false)) == 2 && pa.getOutputDevice (pa.getIoNodeUid (false)) == "EVO",
               "preset A: main OUT pinned");
        check (checks, pb.getOutputChannel (pb.getIoNodeUid (false)) == 0 && pb.getOutputDevice (pb.getIoNodeUid (false)) == "Other",
               "preset B: a binding made on another device is not re-stamped");
        check (checks, pc.getOutputChannel (pc.getIoNodeUid (false)) == 2 && pc.getOutputDevice (pc.getOutputUids()[1]) == "EVO",
               "preset C: the unbound main OUT is pinned, the bound extra OUT is untouched");
        check (checks, ! store.pinLegacyOutputs (2, "Loop-back 1/2", "EVO"), "preset pinning is idempotent");

        const auto reloaded = PresetStore::fromXml (store.toXml());
        const auto ra = reloaded.getPresetDocument (0);
        check (checks, ra.getOutputChannel (ra.getIoNodeUid (false)) == 2, "pinned bindings survive the preset XML round trip");
    }

    std::cout << (failures == 0 ? "  outputs ok" : "  outputs FAILED") << std::endl;
    return failures;
}
