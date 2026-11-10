#pragma once

#include "ldc/SimplifiedPAG.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <utility>

namespace SVF {
class SVFIR;
class CallGraph;
} // namespace SVF

namespace ldc::frontend {

struct BuildStats {
    std::size_t variantGeps = 0;
    std::size_t skippedEdges = 0;
    std::size_t nonPointerEdges = 0;
    std::size_t indirectEdges = 0;
};

struct FrontendResult {
    SimplifiedPAG graph;
    BuildStats stats;
    std::size_t svfNodeCount = 0;
    // Sorted, unique caller/callee pairs using LLVM linkage names.
    std::vector<std::pair<std::string, std::string>> calls;
    // Andersen's points-to sets of the graph's variables, by node index, with an object's fields
    // folded into the object.
    std::map<std::uint32_t, std::set<std::uint32_t>> andersen;

    void printSummary(std::ostream& out) const;
};

// Loads LLVM IR, runs Andersen, and copies the results before releasing SVF/LLVM.
// Uses SVF's process-global state; must not overlap another active SVF session.
FrontendResult analyzeModules(const std::vector<std::string>& modules);

// Copies the SVF graph and metadata into an independently owned graph.
SimplifiedPAG buildSimplifiedPAG(SVF::SVFIR& pag, const SVF::CallGraph& callGraph,
                                 BuildStats& stats);

} // namespace ldc::frontend
