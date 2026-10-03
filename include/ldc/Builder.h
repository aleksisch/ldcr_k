// Builder — SVFIR → LDGraph (PLAN.md §2.1, milestone M1).

#pragma once

#include "ldc/LDGraph.h"

#include <cstddef>
#include <iosfwd>

namespace SVF {
class SVFIR;
class CallGraph;
} // namespace SVF

namespace ldc {

/// Where the graph is less precise than the program (printed after the graph summary).
struct BuildStats {
    /// geps with a variable struct field: loads and stores through them use field `*`
    std::size_t variantGeps = 0;
    /// statements touching constants, SVF dummies, SVF field objects or intrinsics: dropped
    std::size_t skippedStmts = 0;
    /// call/return edges taken from SVF Andersen's context-insensitive call graph
    std::size_t indirectEdges = 0;
    /// virtual call sites without a declared class: CHA is not filtered by it
    std::size_t sitesWithoutDeclaredType = 0;

    void print(std::ostream& os) const;
};

/// The graph for one analysis: only the virtual-call edges `mode` uses (see Mode).
LDGraph buildLDGraph(SVF::SVFIR& pag, const SVF::CallGraph& callGraph, Mode mode,
                     BuildStats& stats);

} // namespace ldc
