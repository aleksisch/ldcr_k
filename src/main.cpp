// LLVM/SVF driver for constructing and inspecting a call graph.

#include "ldc/Grammar.h"
#include "ldc/PagPasses.h"
#include "ldc/SVFFrontend.h"
#include "CommandLine.h"
#include "Facts.h"
#include "PagDot.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <llvm/Demangle/Demangle.h>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

int main(int argc, char** argv) {
    const auto parsed = ldc::parseCommandLine(argc, argv);
    if (const auto* exitCode = std::get_if<int>(&parsed)) return *exitCode;
    const auto& options = std::get<ldc::CommandLine>(parsed);

    const auto result = ldc::frontend::analyzeModules(options.modules);
    result.printSummary(std::cout);
    bool ok = true;
    if (!options.programDot.empty()) {
        std::ofstream out(options.programDot);
        result.graph.dumpDot(out);
        out.close();
        if (!out) {
            std::cerr << "Cannot write program graph: " << options.programDot << "\n";
            ok = false;
        }
    }
    if (!options.pointsTo && options.facts.empty()) return ok ? 0 : 1;

    ldc::cfl::Options cfl;
    if (options.svfSolver) cfl.solver = ldc::cfl::Options::Solver::Svf;
    const auto start = std::chrono::steady_clock::now();
    // L_D over the functions SVF's call graph (Andersen's) reaches from main.
    std::set<std::string> functions{"", "main"};
    for (bool grown = true; grown;) {
        grown = false;
        for (const auto& [caller, callee] : result.calls)
            if (functions.count(llvm::demangle(caller)))
                grown |= functions.insert(llvm::demangle(callee)).second;
    }
    auto pag = ldc::postProcessPAG(result.graph, functions);
    if (options.cppHeap) ldc::addObjectFields(pag);
    auto grammar = ldc::grammar::ld(pag);
    if (options.cppHeap) grammar = ldc::grammar::ldCpp(pag);
    if (options.refine) {
        std::vector<ldc::cfl::Language> all{{grammar, "S"}, {ldc::grammar::lr(), "S"}};
        if (options.matchCalls) all.push_back({ldc::grammar::lc(), "S"});
        const auto removed = ldc::refinePAG(pag, all, cfl);
        std::cout << "Refinement: removed " << removed << " edges\n";
    }
    // The --path queries: a path in each language on its own, printed and drawn in --pag-dot.
    std::vector<ldc::DrawnPath> drawn;
    std::string legend;
    if (!options.paths.empty()) {
        const auto names =
            ldc::nodeNames(pag, options.source.empty() ? options.pagDotCode : options.source);
        const auto find = [&](const std::string& name) -> std::optional<std::uint32_t> {
            for (std::uint32_t id = 0; id < names.size(); ++id)
                if (names[id] == name) return id;
            std::cerr << "No node named " << name << "\n";
            return std::nullopt;
        };
        const char* const colors[] = {"red",    "blue",  "darkgreen", "darkorange",
                                      "purple", "brown", "magenta",   "teal"};
        const bool aliasX = std::any_of(grammar.begin(), grammar.end(),
                                        [](const auto& rule) { return rule.lhs == "ALIASX"; });
        for (char language : options.languages) {
            auto languageGrammar = grammar;
            if (language == 'C') languageGrammar = ldc::grammar::lc();
            if (language == 'R') languageGrammar = ldc::grammar::lr();
            for (std::size_t q = 0; q < options.paths.size(); ++q) {
                // from:to, split at a ':' that is not part of a "::".
                const auto& query = options.paths[q];
                std::size_t colon = std::string::npos;
                for (std::size_t i = 0; i < query.size() && colon == std::string::npos; ++i)
                    if (query[i] == ':' && (i == 0 || query[i - 1] != ':') &&
                        (i + 1 == query.size() || query[i + 1] != ':'))
                        colon = i;
                const auto from = colon == std::string::npos ? std::nullopt
                                                             : find(query.substr(0, colon));
                const auto to = colon == std::string::npos ? std::nullopt
                                                           : find(query.substr(colon + 1));
                if (!from || !to) {
                    std::cerr << "Bad --path " << query << "\n";
                    ok = false;
                    continue;
                }
                // An object and a variable: points-to (S); two variables: alias.
                const bool alias = pag.nodes()[*from].kind != ldc::frontend::NodeKind::Obj;
                const std::string start =
                    language == 'D' && alias ? (aliasX ? "ALIASX" : "ALIAS") : "S";
                const auto found = ldc::cfl::paths(pag, languageGrammar, start, {{*from, *to}});
                std::cout << "Path " << query << " in L_" << language << ":";
                if (!found[0]) {
                    std::cout << " none\n";
                    continue;
                }
                std::cout << "\n";
                for (auto e : *found[0]) {
                    const auto& edge = pag.edges()[e];
                    std::cout << "  " << names[edge.src.value] << " --"
                              << ldc::edgeTuple(pag, edge) << "--> " << names[edge.dst.value]
                              << "\n";
                }
                const auto* color = colors[q % std::size(colors)];
                drawn.push_back({*found[0], color, std::string(1, language)});
                if (language == options.languages.front())
                    legend += std::string(color) + ": " + query + "\n";
            }
        }
        legend += "steps: D1, C1, R1, ... in L_D, L_C, L_R; ← walks an edge backwards\n";
    }
    if (!options.pagDot.empty()) {
        std::ofstream out(options.pagDot);
        ldc::writePagDot(out, pag, options.pagDotCode, legend, drawn, &result.graph);
        out.close();
        if (!out) {
            std::cerr << "Cannot write PAG: " << options.pagDot << "\n";
            ok = false;
        }
    }
    auto solved = ldc::cfl::solve(pag, grammar, cfl);
    ldc::PointsTo facts;
    for (const auto& [object, node] : solved["S"])
        if (pag.nodes()[object].kind == ldc::frontend::NodeKind::Obj &&
            pag.nodes()[node].kind == ldc::frontend::NodeKind::Var)
            facts[node].insert(object);
    // An object at an offset (--cpp-heap) stands for its object.
    for (auto& [variable, objects] : facts) {
        std::set<std::uint32_t> whole;
        for (auto o : objects)
            whole.insert(pag.nodes()[o].base.value_or(ldc::frontend::NodeId{o}).value);
        objects = std::move(whole);
    }
    // L_R, and L_C with --match-calls: a fact is kept if its object reaches its variable in each of
    // them (at the endpoints: each language may use its own path).
    std::vector<ldc::cfl::Language> languages{{ldc::grammar::lr(), "S"}};
    if (options.matchCalls) languages.push_back({ldc::grammar::lc(), "S"});
    const auto realizable = ldc::cfl::solve(pag, languages, cfl);
    ldc::PointsTo kept;
    for (const auto& [variable, objects] : facts)
        for (auto object : objects)
            if (std::binary_search(realizable.begin(), realizable.end(),
                                   std::pair{object, variable}))
                kept[variable].insert(object);
    facts = std::move(kept);
    const std::chrono::duration<double> seconds = std::chrono::steady_clock::now() - start;
    auto total = std::size_t{0};
    for (const auto& entry : facts) total += entry.second.size();
    std::cout << "Points-to: sum |pts| " << total << ", " << seconds.count() << " s\n";
    if (!options.facts.empty()) {
        std::ofstream out(options.facts);
        ldc::writeFacts(out, result.graph, facts, options.source);
        out.close();
        if (!out) {
            std::cerr << "Cannot write facts: " << options.facts << "\n";
            ok = false;
        }
    }
    return ok ? 0 : 1;
}
