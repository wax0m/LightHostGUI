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
            s.state = tree;
    }
    return s;
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
    return doc;
}

void PresetStore::removePreset (int index)
{
    if (index < 0 || index >= getNumPresets())
        return;
    state.removeChild (index, nullptr);
    setActiveIndex ((int) state.getProperty (idActive, 0));   // re-clamp
}
