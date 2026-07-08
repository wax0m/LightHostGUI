#include "GraphDocument.h"

namespace
{
    const Identifier idGraph       ("GRAPH");
    const Identifier idNodes       ("NODES");
    const Identifier idNode        ("NODE");
    const Identifier idConnections ("CONNECTIONS");
    const Identifier idConnection  ("CONNECTION");
    const Identifier idVersion     ("version");
    const Identifier idUid         ("uid");
    const Identifier idType        ("type");
    const Identifier idName        ("name");
    const Identifier idX           ("x");
    const Identifier idY           ("y");
    const Identifier idBypassed    ("bypassed");
    const Identifier idState       ("state");
    const Identifier idGain        ("gain");
    const Identifier idPan         ("pan");
    const Identifier idSrcUid      ("srcUid");
    const Identifier idSrcCh       ("srcCh");
    const Identifier idDstUid      ("dstUid");
    const Identifier idDstCh       ("dstCh");
}

const char* const GraphDocument::typeAudioIn  = "audioIn";
const char* const GraphDocument::typeAudioOut = "audioOut";
const char* const GraphDocument::typePlugin   = "plugin";

GraphDocument::GraphDocument()
{
    state = ValueTree (idGraph);
    state.setProperty (idVersion, 1, nullptr);
    state.appendChild (ValueTree (idNodes), nullptr);
    state.appendChild (ValueTree (idConnections), nullptr);

    addNodeInternal (typeAudioIn,  "Audio Input",  0.05f, 0.5f);
    addNodeInternal (typeAudioOut, "Audio Output", 0.95f, 0.5f);
}

GraphDocument GraphDocument::fromXml (const String& xml)
{
    GraphDocument doc;
    doc.state = ValueTree();   // start invalid; only a well-formed GRAPH tree revives it

    if (auto parsed = parseXML (xml))
    {
        ValueTree tree = ValueTree::fromXml (*parsed);
        if (tree.hasType (idGraph))
            doc.state = tree;   // isValid() still vets nodes/connections/IO below
    }

    return doc;
}

String GraphDocument::toXml() const
{
    if (auto xml = state.createXml())
        return xml->toString();
    return {};
}

bool GraphDocument::isValid() const
{
    return state.hasType (idGraph)
        && state.getChildWithName (idNodes).isValid()
        && state.getChildWithName (idConnections).isValid()
        && getIoNodeUid (true).isNotEmpty()
        && getIoNodeUid (false).isNotEmpty();
}

//==============================================================================
String GraphDocument::addNodeInternal (const String& type, const String& name, float x, float y)
{
    ValueTree node (idNode);
    const String uid = Uuid().toString();
    node.setProperty (idUid, uid, nullptr);
    node.setProperty (idType, type, nullptr);
    node.setProperty (idName, name, nullptr);
    node.setProperty (idX, x, nullptr);
    node.setProperty (idY, y, nullptr);
    node.setProperty (idBypassed, false, nullptr);
    state.getChildWithName (idNodes).appendChild (node, nullptr);
    return uid;
}

String GraphDocument::addPluginNode (const PluginDescription& desc, float x, float y)
{
    const String uid = addNodeInternal (typePlugin, desc.name, x, y);
    ValueTree node = getNodeByUid (uid);
    if (auto descXml = desc.createXml())
        node.appendChild (ValueTree::fromXml (*descXml), nullptr);
    return uid;
}

void GraphDocument::removeNode (const String& uid)
{
    removeConnectionsInvolving (uid);
    ValueTree nodes = state.getChildWithName (idNodes);
    nodes.removeChild (getNodeByUid (uid), nullptr);
}

ValueTree GraphDocument::getNodeByUid (const String& uid) const
{
    return state.getChildWithName (idNodes).getChildWithProperty (idUid, uid);
}

ValueTree GraphDocument::getNodes() const        { return state.getChildWithName (idNodes); }
ValueTree GraphDocument::getConnections() const  { return state.getChildWithName (idConnections); }

String GraphDocument::getIoNodeUid (bool input) const
{
    ValueTree node = state.getChildWithName (idNodes)
                          .getChildWithProperty (idType, input ? typeAudioIn : typeAudioOut);
    return node.getProperty (idUid).toString();
}

void GraphDocument::setBypassed (const String& uid, bool b)
{
    getNodeByUid (uid).setProperty (idBypassed, b, nullptr);
}

bool GraphDocument::isBypassed (const String& uid) const
{
    return (bool) getNodeByUid (uid).getProperty (idBypassed, false);
}

void GraphDocument::setPluginState (const String& uid, const String& base64)
{
    getNodeByUid (uid).setProperty (idState, base64, nullptr);
}

String GraphDocument::getPluginState (const String& uid) const
{
    return getNodeByUid (uid).getProperty (idState).toString();
}

void GraphDocument::setGain (const String& uid, float g)
{
    getNodeByUid (uid).setProperty (idGain, g, nullptr);
}

float GraphDocument::getGain (const String& uid) const
{
    return (float) getNodeByUid (uid).getProperty (idGain, 1.0f);
}

void GraphDocument::setPan (const String& uid, float pan)
{
    getNodeByUid (uid).setProperty (idPan, pan, nullptr);
}

float GraphDocument::getPan (const String& uid) const
{
    return (float) getNodeByUid (uid).getProperty (idPan, 0.0f);
}

void GraphDocument::clearAllPluginStates()
{
    ValueTree nodes = state.getChildWithName (idNodes);
    for (int i = 0; i < nodes.getNumChildren(); ++i)
        nodes.getChild (i).removeProperty (idState, nullptr);
}

std::optional<PluginDescription> GraphDocument::getPluginDescription (const ValueTree& node)
{
    ValueTree pluginTree = node.getChildWithName ("PLUGIN");
    if (! pluginTree.isValid())
        return std::nullopt;

    if (auto xml = pluginTree.createXml())
    {
        PluginDescription desc;
        if (desc.loadFromXml (*xml))
            return desc;
    }
    return std::nullopt;
}

//==============================================================================
void GraphDocument::addConnection (const String& srcUid, int srcCh, const String& dstUid, int dstCh)
{
    ValueTree c (idConnection);
    c.setProperty (idSrcUid, srcUid, nullptr);
    c.setProperty (idSrcCh, srcCh, nullptr);
    c.setProperty (idDstUid, dstUid, nullptr);
    c.setProperty (idDstCh, dstCh, nullptr);
    state.getChildWithName (idConnections).appendChild (c, nullptr);
}

void GraphDocument::clearConnections()
{
    state.getChildWithName (idConnections).removeAllChildren (nullptr);
}

void GraphDocument::removeConnectionsInvolving (const String& uid)
{
    ValueTree connections = state.getChildWithName (idConnections);
    for (int i = connections.getNumChildren(); --i >= 0;)
    {
        ValueTree c = connections.getChild (i);
        if (c.getProperty (idSrcUid).toString() == uid || c.getProperty (idDstUid).toString() == uid)
            connections.removeChild (i, nullptr);
    }
}

//==============================================================================
GraphDocument GraphDocument::migrateFromLegacySettings (PropertiesFile& settings)
{
    GraphDocument doc;   // starts with audioIn/audioOut

    KnownPluginList legacyActive;
    if (auto xml = settings.getXmlValue ("pluginListActive"))
        legacyActive.recreateFromXml (*xml);

    const auto legacyKey = [] (const String& type, const PluginDescription& p)
    {
        return "plugin-" + type + "-" + p.name + p.version + p.pluginFormatName;
    };

    const auto types = legacyActive.getTypes();
    std::vector<std::pair<int, PluginDescription>> ordered;
    for (const auto& p : types)
        ordered.push_back ({ settings.getValue (legacyKey ("order", p)).getIntValue(), p });
    std::sort (ordered.begin(), ordered.end(),
               [] (const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<String> chain;
    chain.push_back (doc.getIoNodeUid (true));
    int index = 0;
    for (const auto& [orderTime, plugin] : ordered)
    {
        ignoreUnused (orderTime);
        const float x = 0.1f + 0.8f * (float) (index + 1) / (float) (ordered.size() + 1);
        const String uid = doc.addPluginNode (plugin, x, 0.5f);
        doc.setBypassed (uid, settings.getBoolValue (legacyKey ("bypass", plugin), false));
        const String stateB64 = settings.getValue (legacyKey ("state", plugin));
        if (stateB64.isNotEmpty())
            doc.setPluginState (uid, stateB64);
        chain.push_back (uid);
        ++index;
    }
    chain.push_back (doc.getIoNodeUid (false));

    for (size_t i = 0; i + 1 < chain.size(); ++i)
    {
        doc.addConnection (chain[i], 0, chain[i + 1], 0);
        doc.addConnection (chain[i], 1, chain[i + 1], 1);
    }

    return doc;
}
