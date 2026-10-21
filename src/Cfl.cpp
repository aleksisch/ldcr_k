#include "ldc/Cfl.h"
#include "CflBackends.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
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

} // namespace ldc::cfl
