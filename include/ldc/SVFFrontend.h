#pragma once

#include "ldc/ProgramGraph.h"
#include <cstddef>

namespace SVF {
class SVFIR;
class CallGraph;
} // namespace SVF

namespace ldc::frontend {

struct BuildStats {
    std::size_t variantGeps = 0;
    std::size_t skippedEdges = 0;
    std::size_t indirectEdges = 0;
};

// Copies the SVF graph and metadata into an independently owned graph.
ProgramGraph buildProgramGraph(SVF::SVFIR& pag, const SVF::CallGraph& callGraph, BuildStats& stats);

} // namespace ldc::frontend
