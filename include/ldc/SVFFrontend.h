#pragma once

#include "ldc/ProgramGraph.h"
#include <cstddef>
#include <unordered_map>

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

struct DebugName {
    std::string function; // demangled lexical scope, even for aliases of global values
    std::string name;
};

// Recover one source name per SVF value. Several source variables may alias the
// same SSA value; in that case the first available name is retained.
std::unordered_map<SvfId, DebugName> debugNames();
int sourceLine(const std::string& sourceLocation);
ProgramGraph buildProgramGraph(SVF::SVFIR& pag, const SVF::CallGraph& callGraph, BuildStats& stats);

} // namespace ldc::frontend
