// Eval — M5: run L_FC_k, kCFA and L_DC_k on one LDGraph and compare them (PLAN.md §4, M5).

#pragma once

#include "ldc/LDGraph.h"

#include <iosfwd>

namespace SVF
{
class PointerAnalysis;
} // namespace SVF

namespace ldc
{

/// Prints one Markdown table (one row per analysis) for context depth k. kCFA is the
/// reference: for each other analysis, "extra" counts objects it adds over kCFA and
/// "missing" objects kCFA has and it lacks (a soundness violation). Returns the number of
/// missing objects over all analyses plus the number of (variable, context) pairs where
/// L_DCR's facts differ from kCFA's (the phase-2 hypothesis L_DCR_k = kCFA).
std::size_t evaluate(const LDGraph& graph, SVF::PointerAnalysis& andersen, unsigned k,
                     std::ostream& os);

} // namespace ldc
