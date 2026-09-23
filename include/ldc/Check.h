// Check — compare solver results with SVF Andersen and with tests/expected/*.json.

#pragma once

#include "ldc/LDGraph.h"
#include "ldc/Solver.h"

#include <cstddef>
#include <iosfwd>
#include <string>

namespace SVF
{
class PointerAnalysis;
} // namespace SVF

namespace ldc
{

/// Per variable node: our PTS vs Andersen's PTS, both as sets of base objects present in
/// LDGraph. Prints each difference; returns their number.
std::size_t compareWithAndersen(const LDGraph& graph, const Solver& solver,
                                SVF::PointerAnalysis& andersen, std::ostream& os);

struct ExpectResult
{
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0; ///< queries for another k
};

/// Checks the queries of `jsonPath` whose "k" equals `k`. The expected set is taken from the
/// key `mode` if the query has it, otherwise from "kcfa" (see tests/expected/README.md).
ExpectResult checkExpected(const std::string& jsonPath, const std::string& sourcePath, int k,
                           const std::string& mode, const LDGraph& graph, const Solver& solver,
                           std::ostream& os);

} // namespace ldc
