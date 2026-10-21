// LLVM/SVF driver for constructing and inspecting a call graph.

#include "ldc/Grammar.h"
#include "ldc/PagPasses.h"
#include "ldc/SVFFrontend.h"
#include "CommandLine.h"
#include "Facts.h"

#include <algorithm>
#include <chrono>
#include <llvm/Demangle/Demangle.h>
#include <fstream>
#include <iostream>
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
    auto grammar = ldc::grammar::ld(pag);
    auto solved = ldc::cfl::solve(pag, grammar, cfl);
    ldc::PointsTo facts;
    for (const auto& [object, node] : solved["S"])
        if (pag.nodes()[object].kind == ldc::frontend::NodeKind::Obj &&
            pag.nodes()[node].kind == ldc::frontend::NodeKind::Var)
            facts[node].insert(object);
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
