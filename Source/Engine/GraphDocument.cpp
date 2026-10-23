#include "GraphDocument.h"
#include "NodeGeometry.h"
#include <cmath>
#include <set>
#include <vector>
#include <map>
#include <utility>

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
    const Identifier idMidi        ("midi");
    const Identifier idMix         ("mix");
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
    state.setProperty (idVersion, currentVersion, nullptr);
    state.appendChild (ValueTree (idNodes), nullptr);
    state.appendChild (ValueTree (idConnections), nullptr);

    addNodeInternal (typeAudioIn,  "Audio Input",  lighthost::geometry::defaultInX,  lighthost::geometry::defaultY);
    addNodeInternal (typeAudioOut, "Audio Output", lighthost::geometry::defaultOutX, lighthost::geometry::defaultY);
}

GraphDocument GraphDocument::fromXml (const String& xml)
{
    GraphDocument doc;
    doc.state = ValueTree();   // start invalid; only a well-formed GRAPH tree revives it

    if (auto parsed = parseXML (xml))
    {
        ValueTree tree = ValueTree::fromXml (*parsed);
        if (tree.hasType (idGraph))
        {
            doc.migratedOnLoad = migrateTree (tree);
            doc.state = tree;   // isValid() still vets nodes/connections/IO below
        }
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

float GraphDocument::sanitise (float v, float lo, float hi, float fallback)
{
    return std::isfinite (v) ? jlimit (lo, hi, v) : fallback;
}

void GraphDocument::setGain (const String& uid, float g)
{
    getNodeByUid (uid).setProperty (idGain, sanitise (g, 0.0f, 2.0f, 1.0f), nullptr);
}

float GraphDocument::getGain (const String& uid) const
{
    return sanitise ((float) getNodeByUid (uid).getProperty (idGain, 1.0f), 0.0f, 2.0f, 1.0f);
}

void GraphDocument::setPan (const String& uid, float pan)
{
    getNodeByUid (uid).setProperty (idPan, sanitise (pan, -1.0f, 1.0f, 0.0f), nullptr);
}

float GraphDocument::getPan (const String& uid) const
{
    return sanitise ((float) getNodeByUid (uid).getProperty (idPan, 0.0f), -1.0f, 1.0f, 0.0f);
}

void GraphDocument::setReceivesMidi (const String& uid, bool b)
{
    getNodeByUid (uid).setProperty (idMidi, b, nullptr);
}

bool GraphDocument::getReceivesMidi (const String& uid) const
{
    return (bool) getNodeByUid (uid).getProperty (idMidi, true);
}

void GraphDocument::setMix (const String& uid, float mix)
{
    getNodeByUid (uid).setProperty (idMix, sanitise (mix, 0.0f, 1.0f, 1.0f), nullptr);
}

float GraphDocument::getMix (const String& uid) const
{
    return sanitise ((float) getNodeByUid (uid).getProperty (idMix, 1.0f), 0.0f, 1.0f, 1.0f);
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
void GraphDocument::setNodePosition (const String& uid, float x, float y)
{
    ValueTree n = getNodeByUid (uid);
    n.setProperty (idX, sanitise (x, 0.0f, lighthost::geometry::kMaxCoord, lighthost::geometry::fallbackX), nullptr);
    n.setProperty (idY, sanitise (y, 0.0f, lighthost::geometry::kMaxCoord, lighthost::geometry::fallbackY), nullptr);
}

float GraphDocument::getNodeX (const String& uid) const
{
    return sanitise ((float) getNodeByUid (uid).getProperty (idX, lighthost::geometry::fallbackX),
                     0.0f, lighthost::geometry::kMaxCoord, lighthost::geometry::fallbackX);
}

float GraphDocument::getNodeY (const String& uid) const
{
    return sanitise ((float) getNodeByUid (uid).getProperty (idY, lighthost::geometry::fallbackY),
                     0.0f, lighthost::geometry::kMaxCoord, lighthost::geometry::fallbackY);
}

bool GraphDocument::hasConnection (const String& srcUid, int srcCh, const String& dstUid, int dstCh) const
{
    ValueTree conns = state.getChildWithName (idConnections);
    for (int i = 0; i < conns.getNumChildren(); ++i)
    {
        ValueTree c = conns.getChild (i);
        if (c.getProperty (idSrcUid).toString() == srcUid && (int) c.getProperty (idSrcCh) == srcCh
         && c.getProperty (idDstUid).toString() == dstUid && (int) c.getProperty (idDstCh) == dstCh)
            return true;
    }
    return false;
}

void GraphDocument::removeConnection (const String& srcUid, int srcCh, const String& dstUid, int dstCh)
{
    ValueTree conns = state.getChildWithName (idConnections);
    for (int i = conns.getNumChildren(); --i >= 0;)
    {
        ValueTree c = conns.getChild (i);
        if (c.getProperty (idSrcUid).toString() == srcUid && (int) c.getProperty (idSrcCh) == srcCh
         && c.getProperty (idDstUid).toString() == dstUid && (int) c.getProperty (idDstCh) == dstCh)
        {
            conns.removeChild (i, nullptr);
            return;
        }
    }
}

// True if following connection edges from fromUid can reach toUid (node level).
bool GraphDocument::pathExists (const String& fromUid, const String& toUid) const
{
    if (fromUid == toUid)
        return true;

    ValueTree conns = state.getChildWithName (idConnections);
    std::set<String> visited;
    std::vector<String> stack { fromUid };

    while (! stack.empty())
    {
        const String cur = stack.back();
        stack.pop_back();
        if (! visited.insert (cur).second)
            continue;

        for (int i = 0; i < conns.getNumChildren(); ++i)
        {
            ValueTree c = conns.getChild (i);
            if (c.getProperty (idSrcUid).toString() == cur)
            {
                const String nxt = c.getProperty (idDstUid).toString();
                if (nxt == toUid) return true;
                stack.push_back (nxt);
            }
        }
    }
    return false;
}

bool GraphDocument::canAddConnection (const String& srcUid, int srcCh, const String& dstUid, int dstCh) const
{
    if (srcUid == dstUid)                       return false;   // no self-connection
    if (srcCh < 0 || srcCh > 1 || dstCh < 0 || dstCh > 1) return false;

    ValueTree srcNode = getNodeByUid (srcUid);
    ValueTree dstNode = getNodeByUid (dstUid);
    if (! srcNode.isValid() || ! dstNode.isValid()) return false;

    if (srcNode.getProperty (idType).toString() == typeAudioOut) return false;   // output is a sink
    if (dstNode.getProperty (idType).toString() == typeAudioIn)  return false;   // input is a source

    if (hasConnection (srcUid, srcCh, dstUid, dstCh)) return false;              // duplicate
    if (pathExists (dstUid, srcUid))                 return false;              // would create a cycle

    return true;
}

//==============================================================================
// GRAPH version 1 stored node x/y NORMALISED (0..1) over a zoom-independent layout area; version 2
// stores the node centre in logical canvas px. The v1 layout constants below are FROZEN here on
// purpose: they describe how a v1 document looked at the default window and must never follow later
// UI changes.
bool GraphDocument::migrateTree (ValueTree& graph)
{
    using namespace lighthost::geometry;

    if (! graph.hasType (idGraph) || (int) graph.getProperty (idVersion, 1) >= 2)
        return false;   // already v2, or from a newer build: never downgrade

    ValueTree nodes = graph.getChildWithName (idNodes);
    if (! nodes.isValid())
        return false;

    struct Item { ValueTree node; bool endpoint; float nx, ny; Point<float> centre; };
    std::vector<Item> items;
    std::vector<size_t> plugins;   // indices into items, document order

    const auto rawNorm = [] (const ValueTree& n, const Identifier& id)
    {
        const var v = n.getProperty (id);
        const float f = v.isVoid() ? 0.5f : (float) v;
        return std::isfinite (f) ? jlimit (0.0f, 1.0f, f) : 0.5f;
    };

    for (int i = 0; i < nodes.getNumChildren(); ++i)
    {
        ValueTree n = nodes.getChild (i);
        const bool isPlugin = n.getProperty (idType).toString() == typePlugin;
        if (isPlugin)
            plugins.push_back (items.size());
        items.push_back ({ n, ! isPlugin, rawNorm (n, idX), rawNorm (n, idY), {} });
    }

    // Serially built or migrated chains stacked every plugin at (0.5, 0.5); spread them left to right.
    int stacked = 0;
    for (auto idx : plugins)
        if (std::abs (items[idx].nx - 0.5f) < 1.0e-3f && std::abs (items[idx].ny - 0.5f) < 1.0e-3f)
            ++stacked;

    const int n = (int) plugins.size();
    if (stacked >= 2)
        for (int i = 0; i < n; ++i)
        {
            items[plugins[(size_t) i]].nx = 0.2f + (float) i * 0.6f / (float) (n - 1);
            items[plugins[(size_t) i]].ny = 0.5f;
        }

    // v1 layout at the default window.
    const int refW = n >= 2 ? jmax (1200, 280 + roundToInt (270.0f * (float) (n - 1) / 0.6f)) : 1200;
    const int refH = 700;
    for (auto& it : items)
        it.centre = { 140.0f + it.nx * (float) (refW - 280), 170.0f + it.ny * (float) (refH - 340) };

    // A window bigger than the reference let v1 layouts be wider than the reference: if the plugin cards
    // overlap here, scale the arrangement up about its top-left until they clear (up to 3x), ...
    // Only card against card is checked: the narrow IN / OUT terminals never hide a card, and the v1
    // default spread already puts the first card flush against IN.
    const auto rectOf = [] (const Item& it, Point<float> c) { return nodeRect (it.endpoint, c).expanded ((float) nodeGap * 0.5f); };
    const auto anyOverlap = [&] (const std::vector<Point<float>>& cs)
    {
        for (size_t a = 0; a < items.size(); ++a)
            for (size_t b = a + 1; b < items.size(); ++b)
                if (! items[a].endpoint && ! items[b].endpoint
                    && rectOf (items[a], cs[a]).intersects (rectOf (items[b], cs[b])))
                    return true;
        return false;
    };

    Point<float> origin { 1.0e9f, 1.0e9f };
    for (const auto& it : items)
    {
        const auto r = nodeRect (it.endpoint, it.centre);
        origin = { jmin (origin.x, r.getX()), jmin (origin.y, r.getY()) };
    }

    std::vector<Point<float>> centres;
    for (const auto& it : items)
        centres.push_back (it.centre);

    if (anyOverlap (centres))
    {
        for (int step = 1; step <= 20; ++step)
        {
            const float f = 1.0f + 0.1f * (float) step;
            for (size_t i = 0; i < items.size(); ++i)
                centres[i] = origin + (items[i].centre - origin) * f;
            if (! anyOverlap (centres))
                break;
        }

        // ... and if a tightly packed layout still overlaps, push nodes right in x order until they clear.
        if (anyOverlap (centres))
        {
            std::vector<size_t> order = plugins;
            std::stable_sort (order.begin(), order.end(), [&] (size_t a, size_t b) { return centres[a].x < centres[b].x; });

            for (size_t k = 1; k < order.size(); ++k)
            {
                const size_t i = order[k];
                for (int guard = 0; guard < 64; ++guard)
                {
                    bool moved = false;
                    for (size_t j = 0; j < k; ++j)
                    {
                        const size_t o = order[j];
                        if (rectOf (items[i], centres[i]).intersects (rectOf (items[o], centres[o])))
                        {
                            const float need = (nodeRect (items[o].endpoint, centres[o]).getWidth()
                                                + nodeRect (items[i].endpoint, centres[i]).getWidth()) * 0.5f + (float) nodeGap;
                            centres[i].x = centres[o].x + need;
                            moved = true;
                        }
                    }
                    if (! moved)
                        break;
                }
            }
        }
    }

    // Shift so no node starts left of or above the canvas origin.
    float minLeft = 0.0f, minTop = 0.0f;
    for (size_t i = 0; i < items.size(); ++i)
    {
        const auto r = nodeRect (items[i].endpoint, centres[i]);
        minLeft = jmin (minLeft, r.getX());
        minTop  = jmin (minTop,  r.getY());
    }

    for (size_t i = 0; i < items.size(); ++i)
    {
        items[i].node.setProperty (idX, sanitise (centres[i].x - minLeft, 0.0f, kMaxCoord, fallbackX), nullptr);
        items[i].node.setProperty (idY, sanitise (centres[i].y - minTop,  0.0f, kMaxCoord, fallbackY), nullptr);
    }

    graph.setProperty (idVersion, currentVersion, nullptr);
    return true;
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
        const float x = lighthost::geometry::defaultInX + (float) ((index + 1) * lighthost::geometry::nodeSlot);
        const String uid = doc.addPluginNode (plugin, x, lighthost::geometry::defaultY);
        doc.setBypassed (uid, settings.getBoolValue (legacyKey ("bypass", plugin), false));
        const String stateB64 = settings.getValue (legacyKey ("state", plugin));
        if (stateB64.isNotEmpty())
            doc.setPluginState (uid, stateB64);
        chain.push_back (uid);
        ++index;
    }
    chain.push_back (doc.getIoNodeUid (false));
    doc.setNodePosition (doc.getIoNodeUid (false),
                         lighthost::geometry::defaultInX + (float) ((index + 1) * lighthost::geometry::nodeSlot),
                         lighthost::geometry::defaultY);

    for (size_t i = 0; i + 1 < chain.size(); ++i)
    {
        doc.addConnection (chain[i], 0, chain[i + 1], 0);
        doc.addConnection (chain[i], 1, chain[i + 1], 1);
    }

    return doc;
}

//==============================================================================
bool GraphDocument::isLinearChain() const
{
    const String in  = getIoNodeUid (true);
    const String out = getIoNodeUid (false);

    ValueTree nodes = state.getChildWithName (idNodes);
    int pluginCount = 0;
    for (int i = 0; i < nodes.getNumChildren(); ++i)
        if (nodes.getChild (i).getProperty (idType).toString() == typePlugin)
            ++pluginCount;

    // Node-level distinct edges (dedupe the two stereo channels of each hop).
    std::set<std::pair<String, String>> edges;
    ValueTree conns = state.getChildWithName (idConnections);
    for (int i = 0; i < conns.getNumChildren(); ++i)
    {
        ValueTree c = conns.getChild (i);
        edges.insert ({ c.getProperty (idSrcUid).toString(), c.getProperty (idDstUid).toString() });
    }

    // Empty chain (no plugins): linear if there are no edges, or just in -> out.
    if (pluginCount == 0)
        return edges.empty() || (edges.size() == 1 && edges.count ({ in, out }) > 0);

    std::map<String, std::vector<String>> outAdj;
    for (const auto& e : edges)
        outAdj[e.first].push_back (e.second);

    // Follow a UNIQUE out-edge from audioIn until audioOut; any fan-out fails.
    std::vector<String> path { in };
    std::set<String> visited { in };
    String cur = in;
    while (cur != out)
    {
        const auto it = outAdj.find (cur);
        if (it == outAdj.end() || it->second.size() != 1)
            return false;                               // dead-end or fan-out
        const String next = it->second.front();
        if (! visited.insert (next).second)
            return false;                               // revisit / cycle
        path.push_back (next);
        cur = next;
    }

    int pluginsOnPath = 0;
    for (const auto& u : path)
        if (getNodeByUid (u).getProperty (idType).toString() == typePlugin)
            ++pluginsOnPath;

    // Every plugin on the single path, and no edges outside it.
    return pluginsOnPath == pluginCount
        && (int) edges.size() == (int) path.size() - 1;
}
