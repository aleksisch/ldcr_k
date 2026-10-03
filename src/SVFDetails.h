#pragma once

#include "ldc/SimplifiedPAG.h"
#include <cstddef>
#include <vector>

namespace SVF {
class SVFIR;
class SVFVar;
class CallGraph;
class CallICFGNode;
} // namespace SVF

namespace ldc::frontend::detail {

// Non-owning extraction results for consumers that build a specialized graph.
// SVF must remain alive while these pointers are used.
struct SvfEdge {
    const SVF::SVFVar* src;
    const SVF::SVFVar* dst;
    Label label;
    std::optional<FieldOffset> field;
    const SVF::CallICFGNode* site = nullptr;
    CallDir dir = CallDir::None;
};
using SvfEdges = std::vector<SvfEdge>;
struct StatementEdges {
    SvfEdges edges;
    std::size_t variantGeps = 0;
};
struct IndirectCalls {
    std::vector<const SVF::CallICFGNode*> sites;
    SvfEdges edges;
};

struct DebugName {
    std::string function; // demangled lexical scope, even for aliases of global values
    std::string name;
};

// Recover one source name per SVF value. Several source variables may alias the
// same SSA value; in that case the first available name is retained.
std::unordered_map<SvfId, DebugName> debugNames();
int sourceLine(const std::string& sourceLocation);
bool relevant(const SVF::SVFVar* var);
StatementEdges statementEdges(SVF::SVFIR& pag);
IndirectCalls indirectCalls(SVF::SVFIR& pag, const SVF::CallGraph& callGraph,
                            bool includeVirtualCalls = true);

} // namespace ldc::frontend::detail
