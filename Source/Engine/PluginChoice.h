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
    // the stored list; menus just don't offer them.
    inline juce::Array<juce::PluginDescription> filterLoadable (const juce::Array<juce::PluginDescription>& types,
                                                                const juce::StringArray& registeredFormatNames)
    {
        juce::Array<juce::PluginDescription> out;
        for (const auto& t : types)
            if (registeredFormatNames.contains (t.pluginFormatName))
                out.add (t);
        return out;
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
