#include "ldc/PagPasses.h"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace ldc {

using frontend::CallDir;
using frontend::CallSiteId;
using frontend::Edge;
using frontend::FieldOffset;
using frontend::Label;
using frontend::NodeId;
using frontend::NodeKind;
using frontend::SimplifiedPAG;

namespace {

// Synthetic receiver fields (Dispatch.cpp): the result at -10, parameter i at -10 - i.
bool isSynthetic(const Edge& edge) { return edge.field && edge.field->value <= -10; }

// The function an edge runs in: the caller for a call or a return, else a variable's, or for a
// global or an object the other end's.
const std::string& functionOf(const SimplifiedPAG& pag, const Edge& e) {
    if (e.dir == CallDir::Enter || e.dir == CallDir::Exit)
        return pag.callSites()[e.callSite->value].caller;
    const auto& dst = pag.nodes()[e.dst.value];
    if (dst.kind != NodeKind::Obj && !dst.function.empty()) return dst.function;
    const auto& src = pag.nodes()[e.src.value];
    static const std::string global;
    return src.kind == NodeKind::Obj ? global : src.function;
}

void splitDispatchedThis(SimplifiedPAG& graph) {
    // this#c for every (callee `this`, dispatch site c).
    std::map<std::pair<std::uint32_t, std::int32_t>, NodeId> copies;
    std::map<std::uint32_t, std::vector<std::pair<std::int32_t, NodeId>>> sites; // this -> copies
    for (const auto& e : graph.edges()) {
        if (e.label != Label::Dispatch) continue;
        const auto key = std::pair{e.dst.value, e.callSite->value};
        if (copies.count(key)) continue;
        auto node = graph.nodes()[e.dst.value];
        node.kind = NodeKind::Receiver;
        node.svfId.reset();
        const auto suffix = "#c" + std::to_string(key.second);
        node.name += suffix;
        if (!node.sourceName.empty()) node.sourceName += suffix;
        const auto copy = graph.addNode(node);
        copies.emplace(key, copy);
        sites[key.first].push_back({key.second, copy});
    }

    std::vector<Edge> edges;
    for (auto e : graph.edges()) {
        if (e.label == Label::Dispatch) {
            e.dst = copies.at({e.dst.value, e.callSite->value});
            edges.push_back(e);
            continue;
        }
        // The callee side of the synthetic fields, shared by all calls: once per dispatch site.
        const bool load = e.label == Label::Load && sites.count(e.src.value);
        const bool store = e.label == Label::Store && sites.count(e.dst.value);
        if (isSynthetic(e) && !e.callSite && (load || store)) {
            for (const auto& [site, copy] : sites.at(load ? e.src.value : e.dst.value)) {
                auto moved = e;
                (load ? moved.src : moved.dst) = copy;
                moved.callSite = CallSiteId{site};
                edges.push_back(moved);
            }
            continue;
        }
        edges.push_back(e);
    }
    for (const auto& [key, copy] : copies)
        edges.push_back({copy, NodeId{key.first}, Label::Assign});
    graph.edges() = std::move(edges);
}

SimplifiedPAG keepFunctions(const SimplifiedPAG& pag, const std::set<std::string>& functions) {
    auto part = pag;
    part.edges().clear();
    for (const auto& e : pag.edges())
        if (functions.count(functionOf(pag, e))) part.addEdge(e);
    return part;
}

void addReversedEdges(SimplifiedPAG& pag) {
    const auto count = pag.edges().size();
    for (std::size_t i = 0; i < count; ++i) {
        auto e = pag.edges()[i];
        std::swap(e.src, e.dst);
        e.reversed = true;
        pag.addEdge(e);
    }
}

} // namespace

SimplifiedPAG postProcessPAG(const SimplifiedPAG& pag, const std::set<std::string>& functions) {
    auto result = keepFunctions(pag, functions);
    splitDispatchedThis(result);
    addReversedEdges(result);
    return result;
}

void addObjectFields(SimplifiedPAG& pag) {
    constexpr std::int32_t kAny = frontend::kAnyField.value;
    constexpr std::uint32_t kFieldLimit = 512; // SVF's MaxFieldLimit, for objects without one
    std::set<std::int32_t> gepFields, accessFields;
    std::map<std::uint32_t, std::optional<NodeId>> objects; // object -> its class's vtable
    for (const auto& e : pag.edges()) {
        if (e.reversed) continue;
        if (e.label == Label::Gep) gepFields.insert(e.field ? e.field->value : kAny);
        if ((e.label == Label::Store || e.label == Label::Load) && e.field && e.field->value >= 0)
            accessFields.insert(e.field->value);
        if (e.label == Label::New) objects.emplace(e.src.value, e.type);
    }
    const auto added = pag.edges().size();
    for (const auto& [object, type] : objects) {
        const auto original = pag.nodes()[object];
        const auto limit = std::min(original.fieldLimit.value_or(kFieldLimit), kFieldLimit);
        // An offset or a field from it, `*` at or beyond the object's field limit.
        const auto shift = [&](std::int32_t offset, std::int32_t field) {
            if (offset == kAny || field == kAny) return kAny;
            return offset + field < static_cast<std::int32_t>(limit) ? offset + field : kAny;
        };
        const auto addNode = [&](NodeKind kind, std::int32_t offset, const std::string& suffix) {
            auto node = original;
            node.kind = kind;
            node.svfId.reset();
            node.base = NodeId{object};
            node.offset = offset;
            node.name += suffix;
            return pag.addNode(node);
        };
        const auto name = [&](std::int32_t offset) {
            return offset == kAny ? std::string("*") : std::to_string(offset);
        };

        // The offsets its geps can reach.
        std::map<std::int32_t, NodeId> at{{0, NodeId{object}}};
        for (std::vector<std::int32_t> work{0}; !work.empty();) {
            const auto offset = work.back();
            work.pop_back();
            for (auto f : gepFields)
                if (const auto to = shift(offset, f); !at.count(to)) {
                    at.emplace(to, addNode(NodeKind::Obj, to, "@" + name(to)));
                    work.push_back(to);
                }
        }
        for (const auto& [offset, node] : at)
            for (auto f : gepFields)
                if (auto it = at.find(shift(offset, f)); it != at.end())
                    pag.addEdge({it->second, node, Label::Shift, FieldOffset{f}, std::nullopt,
                                 CallDir::None, offset == 0 ? type : std::nullopt});

        // Cells: a store through a pointer to (o, a) at field g and a load at (o, a') field g'
        // meet in the cell (o, a + g) when a + g == a' + g'.
        const auto star = addNode(NodeKind::Cell, kAny, ".cell*");
        const auto hub = addNode(NodeKind::Hub, 0, ".hub");
        std::map<std::int32_t, NodeId> cells{{kAny, star}};
        for (const auto& [offset, node] : at) {
            for (auto g : accessFields) {
                const auto absolute = shift(offset, g);
                auto it = cells.find(absolute);
                if (it == cells.end())
                    it = cells
                             .emplace(absolute,
                                      addNode(NodeKind::Cell, absolute, ".cell" + name(absolute)))
                             .first;
                pag.addEdge({it->second, node, Label::Cell, FieldOffset{g}});
                if (absolute != kAny)
                    pag.addEdge({star, node, Label::StarCell, FieldOffset{g}});
                else
                    pag.addEdge({hub, node, Label::Hub, FieldOffset{g}});
            }
            pag.addEdge({star, node, Label::StarCell});
            pag.addEdge({hub, node, Label::Hub});
        }
        for (const auto& [absolute, cell] : cells) pag.addEdge({cell, hub, Label::CellHub});
    }
    for (auto i = added, count = pag.edges().size(); i < count; ++i) {
        auto e = pag.edges()[i];
        std::swap(e.src, e.dst);
        e.reversed = true;
        pag.addEdge(e);
    }
}

} // namespace ldc
