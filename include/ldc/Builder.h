// Builder — SVFIR → LDGraph (PLAN.md §2.1, milestone M1).
//
// M1 wiring of calls:
//   direct calls        SVF CallPE / RetPE        → assign ĉ_c / assign č_c
//   indirect + virtual  Andersen call graph edges → assign ĉ_c / assign č_c
// i.e. virtual calls are wired the L_FC way (paper Fig. 2, [P-VCall]): the
// receiver is passed to `this` like any other argument. M4 replaces this for
// virtual calls with the L_D rules of paper Fig. 6.

#pragma once

#include "ldc/LDGraph.h"

#include <cstddef>
#include <iosfwd>

namespace SVF
{
class SVFIR;
class CallGraph;
} // namespace SVF

namespace ldc
{

/// What the builder could not model precisely. Reported, never silently dropped.
struct BuildStats
{
    std::size_t variantGeps = 0;   ///< gep with a variable offset → field kAnyField
    std::size_t escapingGeps = 0;  ///< gep result used other than as a load/store address
    std::size_t skippedStmts = 0;  ///< statements involving only filtered nodes
    std::size_t indirectEdges = 0; ///< call/return edges added from the Andersen call graph

    void print(std::ostream& os) const;
};

LDGraph buildLDGraph(SVF::SVFIR& pag, const SVF::CallGraph& callGraph, BuildStats& stats);

} // namespace ldc
