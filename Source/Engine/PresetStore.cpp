#include "PresetStore.h"

using namespace juce;

namespace
{
    const Identifier idPresets ("PRESETS");
    const Identifier idPreset  ("PRESET");
    const Identifier idName    ("name");
    const Identifier idActive  ("active");
    const Identifier idVersion ("version");
    const Identifier idGraph   ("GRAPH");
}

PresetStore::PresetStore()
{
    state = ValueTree (idPresets);
    state.setProperty (idVersion, 1, nullptr);
    state.setProperty (idActive, -1, nullptr);
}

PresetStore PresetStore::fromXml (const String& xml)
{
    PresetStore s;
    s.state = ValueTree();                       // start invalid
    if (auto parsed = parseXML (xml))
    {
        ValueTree tree = ValueTree::fromXml (*parsed);
        if (tree.hasType (idPresets))
        {
            for (auto preset : tree)
            {
                auto graph = preset.getChildWithName (idGraph);
                if (graph.isValid() && GraphDocument::migrateTree (graph))
                    s.migratedOnLoad = true;
            }
            s.state = tree;
        }
    }
    return s;
}

GraphDocument PresetStore::emptyPresetDocument()
{
    GraphDocument doc;   // audioIn + main audioOut
    const auto in  = doc.getIoNodeUid (true);
    const auto out = doc.getIoNodeUid (false);
    doc.addConnection (in, 0, out, 0);
    doc.addConnection (in, 1, out, 1);
    return doc;
}

String PresetStore::toXml() const
{
    if (auto xml = state.createXml())
        return xml->toString();
    return {};
}

bool PresetStore::isValid() const { return state.hasType (idPresets); }

int PresetStore::getNumPresets() const { return state.getNumChildren(); }

String PresetStore::getPresetName (int index) const
{
    return state.getChild (index).getProperty (idName).toString();
}

void PresetStore::setPresetName (int index, const String& name)
{
    if (auto c = state.getChild (index); c.isValid())
        c.setProperty (idName, name, nullptr);
}

int PresetStore::getActiveIndex() const
{
    const int a = (int) state.getProperty (idActive, -1);
    return (a >= 0 && a < getNumPresets()) ? a : (getNumPresets() > 0 ? 0 : -1);
}

void PresetStore::setActiveIndex (int index)
{
    state.setProperty (idActive, jlimit (0, jmax (0, getNumPresets() - 1), index), nullptr);
}

int PresetStore::addPreset (const String& name, const GraphDocument& doc)
{
    ValueTree p (idPreset);
    p.setProperty (idName, name, nullptr);
    p.appendChild (doc.state.createCopy(), nullptr);       // deep copy
    state.appendChild (p, nullptr);
    const int index = getNumPresets() - 1;
    if ((int) state.getProperty (idActive, -1) < 0)
        state.setProperty (idActive, index, nullptr);      // first preset becomes active
    return index;
}

void PresetStore::setPresetDocument (int index, const GraphDocument& doc)
{
    ValueTree p = state.getChild (index);
    if (! p.isValid())
        return;
    p.removeChild (p.getChildWithName (idGraph), nullptr);
    p.appendChild (doc.state.createCopy(), nullptr);
}

GraphDocument PresetStore::getPresetDocument (int index) const
{
    ValueTree p = state.getChild (index);
    ValueTree g = p.getChildWithName (idGraph);
    if (! g.isValid())
        return GraphDocument();                            // empty default
    GraphDocument doc;
    doc.state = g.createCopy();                            // deep copy out
    GraphDocument::migrateTree (doc.state);                // no-op unless it is a v1 tree
    return doc;
}

bool PresetStore::pinLegacyOutputs (int outCh, const String& label, const String& device)
{
    bool changed = false;
    for (auto preset : state)
    {
        auto graph = preset.getChildWithName (idGraph);
        if (graph.isValid() && GraphDocument::pinLegacyOutputs (graph, outCh, label, device))
            changed = true;
    }
    return changed;
}

String PresetStore::uniqueCopyName (const String& base) const
{
    auto taken = [this] (const String& name)
    {
        for (int i = 0; i < getNumPresets(); ++i)
            if (getPresetName (i).equalsIgnoreCase (name))
                return true;
        return false;
    };

    String candidate = base + " copy";
    for (int n = 2; taken (candidate); ++n)
        candidate = base + " copy " + String (n);
    return candidate;
}

int PresetStore::duplicatePreset (int index)
{
    if (index < 0 || index >= getNumPresets())
        return -1;

    const int active = getActiveIndex();
    ValueTree copy = state.getChild (index).createCopy();   // deep copy, graph included
    copy.setProperty (idName, uniqueCopyName (getPresetName (index)), nullptr);
    state.addChild (copy, index + 1, nullptr);
    if (active > index)
        state.setProperty (idActive, active + 1, nullptr);   // keep pointing at the same preset
    return index + 1;
}

void PresetStore::removePreset (int index)
{
    if (index < 0 || index >= getNumPresets())
        return;
    const int active = getActiveIndex();
    state.removeChild (index, nullptr);
    // A preset removed before the active one shifts it down; removing the active one hands the slot to
    // its successor (clamped at the end).
    setActiveIndex (index < active ? active - 1 : active);
}

bool PresetStore::movePreset (int from, int to)
{
    const int n = getNumPresets();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to)
        return false;

    const int active = getActiveIndex();
    state.moveChild (from, to, nullptr);
    int newActive = active;
    if (active == from)                        newActive = to;
    else if (from < active && active <= to)    newActive = active - 1;
    else if (to <= active && active < from)    newActive = active + 1;
    state.setProperty (idActive, newActive, nullptr);
    return true;
}
