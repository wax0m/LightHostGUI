//
//  PresetStore.h — a versioned store of named graph documents ("presets"/scenes)
//  plus which one is active. A preset is just a named GraphDocument, so save/load
//  is cheap (deep-copied ValueTrees). Pure data: no audio, no device — fully unit
//  testable. The app persists this alongside settings (store.toXml()) and switches
//  presets by feeding getPresetDocument() into GraphController::loadDocument().
//
//    <PRESETS version="1" active="0">
//      <PRESET name="Vocal Chain"><GRAPH .../></PRESET>
//      <PRESET name="Drum Bus">   <GRAPH .../></PRESET>
//    </PRESETS>
//

#pragma once

#include <JuceHeader.h>
#include "GraphDocument.h"

class PresetStore
{
public:
    PresetStore();                                        // empty; active = -1

    static PresetStore fromXml (const juce::String& xml); // check isValid()
    juce::String toXml() const;
    bool isValid() const;

    int getNumPresets() const;
    juce::String getPresetName (int index) const;
    void setPresetName (int index, const juce::String& name);

    int  getActiveIndex() const;                          // -1 when empty
    void setActiveIndex (int index);                      // clamped to a valid preset

    // Documents are DEEP-copied in and out, so the store and the live graph never
    // share ValueTree data.
    int  addPreset (const juce::String& name, const GraphDocument&);  // returns new index
    void setPresetDocument (int index, const GraphDocument&);         // overwrite (== "save")
    GraphDocument getPresetDocument (int index) const;               // load (default doc if OOR)
    void removePreset (int index);

    juce::ValueTree state;   // PRESETS
};
