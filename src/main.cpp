// LLVM/SVF driver for constructing and inspecting a call graph.

#include "ldc/SVFFrontend.h"
#include "Graphs/CallGraph.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

#include <fstream>
#include <iostream>
#include <set>
#include <utility>
#include <string>
#include <vector>

using namespace SVF;

namespace {
const Option<std::string> GraphOut("program-dot", "Write the common pointer-flow graph to DOT", "");
}

int main(int argc, char** argv) {
    std::vector<std::string> modules =
        OptionBase::parseOptions(argc, argv, "ldc: LLVM/SVF call graph", "[options] <input.ll>");
    if (modules.empty()) {
        std::cerr << "usage: ldc [options] <input.ll>\n";
        return 1;
    }

    // Build the module in memory without writing preprocessing bitcode files.
    LLVMModuleSet::buildSVFModule(modules);

    SVFIRBuilder builder;
    SVFIR* pag = builder.build();
    Andersen* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    std::cout << "SVFIR: " << pag->getTotalNodeNum() << " nodes\n";
    // Andersen augments the initial graph with resolved indirect and virtual calls.
    // Print stable, unique caller/callee pairs; SVF retains the call-site information.
    std::set<std::pair<std::string, std::string>> edges;
    for (const auto& entry : *ander->getCallGraph()) {
        const CallGraphNode* caller = entry.second;
        for (const CallGraphEdge* edge : caller->getOutEdges())
            edges.emplace(caller->getName(), edge->getDstNode()->getName());
    }
    for (const auto& edge : edges)
        std::cout << "call: " << edge.first << " -> " << edge.second << "\n";
    ldc::frontend::BuildStats stats;
    const auto graph = ldc::frontend::buildSimplifiedPAG(*pag, *ander->getCallGraph(), stats);
    graph.printSummary(std::cout);
    std::cout << "Build: " << stats.variantGeps << " variable-offset geps, " << stats.skippedEdges
              << " skipped edges, " << stats.indirectEdges << " indirect call/return edges\n";
    bool ok = true;
    if (!GraphOut().empty()) {
        std::ofstream out(GraphOut());
        graph.dumpDot(out);
        out.close();
        if (!out) {
            std::cerr << "Cannot write program graph: " << GraphOut() << "\n";
            ok = false;
        }
    }
    AndersenWaveDiff::releaseAndersenWaveDiff();
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
    return ok ? 0 : 1;
}
