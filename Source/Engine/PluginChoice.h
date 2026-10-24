//
//  PluginChoice.h — pure helpers behind the plugin menus: which known plugins can actually be
//  loaded by this build, and which of them to suggest as the replacement of a given plugin.
//  Header-only and headless-testable; the menus only add the PopupMenu plumbing.
//

#pragma once

#include <JuceHeader.h>
#include "Vst2State.h"
#include <vector>

namespace lighthost::choice
{
    // Plugins whose format has no registered AudioPluginFormat (the VST2 entries of the old
    // upstream scans) can't be loaded: they would only ever become empty stand-ins. They stay in
    // the stored list; menus just don't offer them. The same goes for suspended files (a plugin that killed
    // the last start): the description stays in the list, but ADD, the tray and Replace don't offer it.
    inline juce::Array<juce::PluginDescription> filterLoadable (const juce::Array<juce::PluginDescription>& types,
                                                                const juce::StringArray& registeredFormatNames,
                                                                const juce::StringArray& suspendedFiles = {})
    {
        juce::Array<juce::PluginDescription> out;
        for (const auto& t : types)
            if (registeredFormatNames.contains (t.pluginFormatName)
                && ! (t.fileOrIdentifier.isNotEmpty() && suspendedFiles.contains (t.fileOrIdentifier, true)))
                out.add (t);
        return out;
    }

    // Result of a pick in the canvas ADD menu. Instruments use ids 1..N, outputs outputIdBase.. (one per device
    // pair), everything else is a KnownPluginList id (they start at 0x324503f4). Decoded by EXACT ranges: a
    // plain "id >= base" test swallows the plugin ids (B-R4).
    struct AddMenuChoice
    {
        enum class Kind { none, instrument, output, effect };
        Kind kind = Kind::none;
        int index = -1;   // instrument index, device pair index, or index into the effects list
    };

    inline AddMenuChoice decodeAddMenuId (int id, int numInstruments, int outputIdBase, int numOutputPairs,
                                          const juce::Array<juce::PluginDescription>& effects)
    {
        if (id <= 0)
            return {};
        if (id <= numInstruments)
            return { AddMenuChoice::Kind::instrument, id - 1 };
        if (id >= outputIdBase && id < outputIdBase + numOutputPairs)
            return { AddMenuChoice::Kind::output, id - outputIdBase };
        if (const int idx = juce::KnownPluginList::getIndexChosenByMenu (effects, id); idx >= 0)
            return { AddMenuChoice::Kind::effect, idx };
        return {};
    }

    // Indices into `candidates`, best first: the same normalised name (Graillon 2-64 -> Graillon 2),
    // then anything else by the same manufacturer. A candidate with the old plugin's own file is skipped.
    inline std::vector<int> suggestReplacements (const juce::PluginDescription& old,
                                                 const juce::Array<juce::PluginDescription>& candidates)
    {
        const auto key = lighthost::vst2::normaliseName (old.name, old.manufacturerName);
        const auto maker = old.manufacturerName.trim();

        std::vector<int> byName, byMaker;
        for (int i = 0; i < candidates.size(); ++i)
        {
            const auto& c = candidates.getReference (i);
            if (c.fileOrIdentifier == old.fileOrIdentifier && c.pluginFormatName == old.pluginFormatName)
                continue;

            if (key.isNotEmpty() && lighthost::vst2::normaliseName (c.name, c.manufacturerName) == key)
                byName.push_back (i);
            else if (maker.isNotEmpty() && c.manufacturerName.trim().equalsIgnoreCase (maker))
                byMaker.push_back (i);
        }

        byName.insert (byName.end(), byMaker.begin(), byMaker.end());
        return byName;
    }
}
