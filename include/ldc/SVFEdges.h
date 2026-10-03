#pragma once

#include "ldc/ProgramGraph.h"
#include <cstddef>
#include <vector>

namespace SVF {
class SVFIR;
class SVFVar;
class CallGraph;
class CallICFGNode;
} // namespace SVF

namespace ldc::frontend {

// Non-owning extraction results for consumers that build a specialized graph.
// SVF must remain alive while these pointers are used.
struct SvfEdge {
    const SVF::SVFVar* src;
    const SVF::SVFVar* dst;
    Label label;
    FieldId field = kNoField;
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

bool relevant(const SVF::SVFVar* var);
StatementEdges statementEdges(SVF::SVFIR& pag);
IndirectCalls indirectCalls(SVF::SVFIR& pag, const SVF::CallGraph& callGraph,
                            bool includeVirtualCalls = true);

} // namespace ldc::frontend
