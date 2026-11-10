#include "ldc/PagPasses.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

namespace ldc {

using frontend::CallDir;
using frontend::CallSiteId;
using frontend::Edge;
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

std::size_t refinePAG(SimplifiedPAG& pag, const std::vector<cfl::Language>& languages,
                      const cfl::Options& options) {
    // The languages take turns: each is solved on the graph the previous ones pruned, keeps the
    // pairs all of them found so far, and drops the edges on none of its paths for those pairs.
    // A pair lost on a smaller graph stays lost, so the pairs only shrink; a full turn that
    // removes nothing is the same fixpoint as pruning by all languages at once.
    std::size_t removed = 0;
    std::optional<cfl::Pairs> pairs;
    for (std::size_t turn = 0, quiet = 0; quiet < languages.size(); ++turn) {
        const auto& language = languages[turn % languages.size()];
        const auto solved = cfl::solve(pag, language.grammar, options);
        auto found = solved.at(language.start);
        std::erase_if(found, [&](const auto& pair) {
            return pag.nodes()[pair.first].kind != NodeKind::Obj ||
                   pag.nodes()[pair.second].kind != NodeKind::Var;
        });
        std::sort(found.begin(), found.end());
        found.erase(std::unique(found.begin(), found.end()), found.end());
        if (pairs) {
            cfl::Pairs both;
            std::set_intersection(pairs->begin(), pairs->end(), found.begin(), found.end(),
                                  std::back_inserter(both));
            found = std::move(both);
        }
        pairs = std::move(found);
        const auto used = cfl::usedEdges(pag, language.grammar, solved, language.start, *pairs);
        std::vector<Edge> kept;
        for (std::size_t e = 0; e < used.size(); ++e)
            if (used[e]) kept.push_back(pag.edges()[e]);
        if (kept.size() == pag.edges().size()) {
            ++quiet;
            continue;
        }
        quiet = 1;
        removed += pag.edges().size() - kept.size();
        pag.edges() = std::move(kept);
    }
    return removed;
}

} // namespace ldc
