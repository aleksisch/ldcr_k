#include "ldc/Cfl.h"
#include "CflBackends.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ldc::cfl {
namespace {

// The grammar without rules between nonterminals (lhs -> A), which the solvers do not take: each
// becomes a copy of every other rule of A (and of the nonterminals A reaches that way) with lhs
// as its left-hand side.
Grammar withoutUnitRules(const Grammar& grammar) {
    std::set<std::string> nonterminals;
    for (const auto& rule : grammar) nonterminals.insert(rule.lhs);
    std::map<std::string, std::set<std::string>> units;
    Grammar out;
    for (const auto& rule : grammar)
        if (rule.b.empty() && nonterminals.count(rule.a))
            units[rule.lhs].insert(rule.a);
        else
            out.push_back(rule);
    const auto direct = out.size();
    for (const auto& [lhs, unused] : units) {
        std::set<std::string> reached;
        std::vector<std::string> work{lhs};
        while (!work.empty()) {
            const auto name = work.back();
            work.pop_back();
            if (auto it = units.find(name); it != units.end())
                for (const auto& a : it->second)
                    if (reached.insert(a).second) work.push_back(a);
        }
        for (std::size_t i = 0; i < direct; ++i)
            if (reached.count(out[i].lhs)) out.push_back({lhs, out[i].a, out[i].b});
    }
    return out;
}

// The PAG's edges as terminals (Cfl.h): each edge once by its label and once by its call site.
Graph terminals(const frontend::SimplifiedPAG& pag) {
    using frontend::CallDir;
    using frontend::Label;
    constexpr std::int32_t kSynthetic = -10; // synthetic receiver fields (Dispatch.cpp) and below
    Graph graph{pag.nodes().size(), {}};
    std::map<std::pair<std::int32_t, std::int32_t>, std::int32_t> argumentKeys;
    for (const auto& e : pag.edges()) {
        const std::string r = e.reversed ? "_r" : "";
        const auto field = e.field ? e.field->value : -1;
        const auto site = e.callSite ? e.callSite->value : -1;
        const auto type = e.type ? std::to_string(e.type->value) : "";
        const auto add = [&](const std::string& label, std::int32_t index = -1) {
            graph.edges.push_back({e.src.value, e.dst.value, label, index});
            graph.edges.back().pagEdge = static_cast<std::size_t>(&e - pag.edges().data());
        };
        switch (e.label) {
        case Label::New: add("new" + type + r); break;
        case Label::Assign: add("assign" + r); break;
        case Label::Gep: add("gep" + r, field + 2); break;
        case Label::Store:
        case Label::Load: {
            const std::string name = e.label == Label::Store ? "store" : "load";
            if (field == frontend::kAnyField.value) {
                add(name + "Any" + r);
            } else if (field > kSynthetic) {
                add(name + r, field);
            } else if (e.callSite) {
                const auto key = argumentKeys.emplace(
                    std::pair{field, site}, static_cast<std::int32_t>(argumentKeys.size()));
                add(name + "Arg" + r, key.first->second);
            }
            break;
        }
        case Label::Dispatch: add("dispatch" + type + r); break;
        case Label::Shift: add("shift" + type + r, field + 2); break;
        case Label::Cell: add("cell" + r, field); break;
        case Label::StarCell:
            if (e.field)
                add("starCell" + r, field);
            else
                add("starCellAny" + r);
            break;
        case Label::Hub:
            if (e.field)
                add("hub" + r, field);
            else
                add("hubAny" + r);
            break;
        case Label::CellHub: add("cellHub" + r); break;
        }
        const bool call = e.callSite && (e.label == Label::Dispatch || e.dir == CallDir::Enter);
        const bool ret = e.callSite && e.dir == CallDir::Exit;
        const bool open = e.callSite && e.dir == CallDir::DispatchEnter;
        const bool close = e.callSite && e.dir == CallDir::DispatchExit;
        if (call || ret)
            add(call != e.reversed ? "call" : "ret", site);
        else if (open || close)
            add(open != e.reversed ? "bo" : "bc", site);
        else
            add("e");
    }
    return graph;
}

} // namespace

std::map<std::string, Pairs> solve(const frontend::SimplifiedPAG& pag, const Grammar& grammar,
                                   const Options& options) {
    const auto graph = terminals(pag);
    const auto rules = withoutUnitRules(grammar);
    switch (options.solver) {
    case Options::Solver::LAGraph: return solveWithLAGraph(graph, rules);
    case Options::Solver::Svf: return solveWithSvf(graph, rules);
    }
    throw std::invalid_argument("Unknown CFL solver");
}

Pairs solve(const frontend::SimplifiedPAG& pag, const std::vector<Language>& languages,
            const Options& options) {
    Pairs result;
    for (std::size_t i = 0; i < languages.size(); ++i) {
        const auto& language = languages[i];
        auto pairs = solve(pag, language.grammar, options)[language.start];
        std::sort(pairs.begin(), pairs.end());
        pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
        if (i == 0) {
            result = std::move(pairs);
            continue;
        }
        Pairs both;
        std::set_intersection(result.begin(), result.end(), pairs.begin(), pairs.end(),
                              std::back_inserter(both));
        result = std::move(both);
    }
    return result;
}

std::vector<std::optional<std::vector<std::size_t>>> paths(const frontend::SimplifiedPAG& pag,
                                                           const Grammar& grammar,
                                                           const std::string& start,
                                                           const Pairs& queries) {
    const auto graph = terminals(pag);
    auto found = findPaths(graph, grammar, start, queries);
    for (auto& path : found)
        if (path)
            for (auto& edge : *path) edge = graph.edges[edge].pagEdge;
    return found;
}

namespace {

bool isFamilyName(const std::string& name) {
    return name.size() > 2 && name.compare(name.size() - 2, 2, "_i") == 0;
}

std::uint64_t pairKey(std::uint32_t u, std::uint32_t v) {
    return (static_cast<std::uint64_t>(u) << 32) | v;
}

// The pairs of a family member or a symbol, sorted: by source with equal_range, tested with
// binary_search.
using Sorted = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

} // namespace

std::vector<bool> usedEdges(const frontend::SimplifiedPAG& pag, const Grammar& grammar,
                            const std::map<std::string, Pairs>& solved, const std::string& start,
                            const Pairs& pairs) {
    const auto graph = terminals(pag);
    const auto rules = withoutUnitRules(grammar);
    // Symbols and their members (index -1 for a symbol without one) by number.
    std::map<std::string, int> symbols;
    const auto symbol = [&](const std::string& name) {
        return symbols.emplace(name, static_cast<int>(symbols.size())).first->second;
    };
    for (const auto& rule : rules)
        for (const auto* name : {&rule.lhs, &rule.a, &rule.b})
            if (!name->empty()) symbol(*name);
    std::unordered_map<std::uint64_t, int> members; // (symbol, index + 1) -> member
    std::vector<Sorted> relations;
    std::vector<std::set<std::int32_t>> indices; // symbol -> its members' indices
    const auto member = [&](int s, std::int32_t index) {
        const auto key = (static_cast<std::uint64_t>(s) << 32) | static_cast<std::uint32_t>(index + 1);
        const auto [it, added] = members.emplace(key, static_cast<int>(relations.size()));
        if (added) relations.emplace_back();
        return it->second;
    };
    for (const auto& [name, found] : solved) {
        const auto open = name.find('[');
        const auto s = symbol(name.substr(0, open));
        std::int32_t index = -1;
        if (open != std::string::npos) {
            index = std::stoi(name.substr(open + 1));
            if (indices.size() <= static_cast<std::size_t>(s)) indices.resize(s + 1);
            indices[s].insert(index);
        }
        relations[member(s, index)].assign(found.begin(), found.end());
    }
    // Terminals: an edge is a member of its label's family if the grammar has one.
    std::vector<int> edgeMembers(graph.edges.size(), -1);
    for (std::size_t e = 0; e < graph.edges.size(); ++e) {
        const auto& edge = graph.edges[e];
        int s = -1;
        std::int32_t index = -1;
        if (const auto it = symbols.find(edge.label + "_i"); it != symbols.end()) {
            s = it->second;
            index = edge.index;
        } else if (const auto it = symbols.find(edge.label); it != symbols.end()) {
            s = it->second;
        } else {
            continue;
        }
        if (index >= 0) {
            if (indices.size() <= static_cast<std::size_t>(s)) indices.resize(s + 1);
            indices[s].insert(index);
        }
        edgeMembers[e] = member(s, index);
        relations[edgeMembers[e]].push_back({edge.src, edge.dst});
    }
    for (auto& relation : relations) {
        std::sort(relation.begin(), relation.end());
        relation.erase(std::unique(relation.begin(), relation.end()), relation.end());
    }
    indices.resize(symbols.size());

    struct Compiled {
        int a, b; // -1: no b
        bool familyA, familyB, familyLhs;
    };
    std::vector<std::vector<Compiled>> byLhs(symbols.size());
    for (const auto& rule : rules)
        byLhs[symbols.at(rule.lhs)].push_back(
            {symbols.at(rule.a), rule.b.empty() ? -1 : symbols.at(rule.b), isFamilyName(rule.a),
             isFamilyName(rule.b), isFamilyName(rule.lhs)});
    // A member's symbol and index, by member.
    std::vector<std::pair<int, std::int32_t>> memberOf(relations.size());
    for (const auto& [key, m] : members)
        memberOf[m] = {static_cast<int>(key >> 32), static_cast<std::int32_t>(key & 0xffffffff) - 1};
    const auto find = [&](int s, std::int32_t index) {
        const auto key = (static_cast<std::uint64_t>(s) << 32) | static_cast<std::uint32_t>(index + 1);
        const auto it = members.find(key);
        return it == members.end() ? -1 : it->second;
    };
    const auto contains = [&](int m, std::uint32_t u, std::uint32_t v) {
        return std::binary_search(relations[m].begin(), relations[m].end(), std::pair{u, v});
    };
    // Each relation by target, (v, u) sorted; and for each family, the indices of its members with
    // a pair from (`from`) or to (`to`) a node.
    std::vector<Sorted> columns(relations.size());
    std::vector<std::unordered_map<std::uint32_t, std::vector<std::int32_t>>> from(symbols.size()),
        to(symbols.size());
    for (std::size_t m = 0; m < relations.size(); ++m) {
        for (const auto& [u, v] : relations[m]) columns[m].push_back({v, u});
        std::sort(columns[m].begin(), columns[m].end());
        const auto [s, index] = memberOf[m];
        if (index < 0) continue;
        for (const auto& [u, v] : relations[m]) {
            if (from[s][u].empty() || from[s][u].back() != index) from[s][u].push_back(index);
            if (to[s][v].empty() || to[s][v].back() != index) to[s][v].push_back(index);
        }
    }
    const auto at = [](const auto& family, std::uint32_t node) -> std::span<const std::int32_t> {
        const auto it = family.find(node);
        if (it == family.end()) return {};
        return it->second;
    };
    const auto row = [](const Sorted& relation, std::uint32_t u) {
        return std::equal_range(relation.begin(), relation.end(), std::pair{u, 0u},
                                [](const auto& x, const auto& y) { return x.first < y.first; });
    };

    // The facts on some derivation of a pair, top-down from the pairs.
    std::vector<std::unordered_set<std::uint64_t>> used(relations.size());
    std::vector<std::tuple<int, std::uint32_t, std::uint32_t>> work;
    const auto use = [&](int m, std::uint32_t u, std::uint32_t v) {
        if (used[m].insert(pairKey(u, v)).second) work.emplace_back(m, u, v);
    };
    if (const auto it = symbols.find(start); it != symbols.end())
        if (const auto m = find(it->second, -1); m >= 0)
            for (const auto& [u, v] : pairs)
                if (contains(m, u, v)) use(m, u, v);
    while (!work.empty()) {
        const auto [m, u, v] = work.back();
        work.pop_back();
        const auto [s, index] = memberOf[m];
        for (const auto& rule : byLhs[s]) {
            // The rule's index: the fact's, or, if it has none, those of its first family's
            // members with a pair from u (or of the second's with a pair to v).
            std::span<const std::int32_t> candidates{&index, 1};
            if (!rule.familyLhs && rule.familyA) candidates = at(from[rule.a], u);
            else if (!rule.familyLhs && rule.familyB) candidates = at(to[rule.b], v);
            for (auto i : candidates) {
                const auto a = find(rule.a, rule.familyA ? i : -1);
                if (a < 0) continue;
                if (rule.b < 0) {
                    if (contains(a, u, v)) use(a, u, v);
                    continue;
                }
                const auto b = find(rule.b, rule.familyB ? i : -1);
                if (b < 0) continue;
                // The middles w of a(u, w) b(w, v), from the shorter of a's row at u and b's
                // column at v.
                const auto first = row(relations[a], u);
                const auto second = row(columns[b], v);
                if (first.second - first.first <= second.second - second.first) {
                    for (auto it = first.first; it != first.second; ++it)
                        if (contains(b, it->second, v)) {
                            use(a, u, it->second);
                            use(b, it->second, v);
                        }
                } else {
                    for (auto it = second.first; it != second.second; ++it)
                        if (contains(a, u, it->second)) {
                            use(a, u, it->second);
                            use(b, it->second, v);
                        }
                }
            }
        }
    }

    std::vector<bool> result(pag.edges().size(), false);
    for (std::size_t e = 0; e < graph.edges.size(); ++e) {
        const auto m = edgeMembers[e];
        if (m >= 0 && used[m].count(pairKey(graph.edges[e].src, graph.edges[e].dst)))
            result[graph.edges[e].pagEdge] = true;
    }
    return result;
}

} // namespace ldc::cfl
