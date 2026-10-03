#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ldc::frontend {

using NodeId = std::uint32_t;
using SvfId = std::uint32_t;
using FieldId = std::int32_t;
using CallSiteId = std::int32_t;
inline constexpr SvfId kNoSvfId = 0xffffffffu;
inline constexpr FieldId kNoField = -1;
inline constexpr FieldId kAnyField = -2;
inline constexpr CallSiteId kNoCallSite = -1;

enum class NodeKind { Var, Obj };
enum class Label { New, Assign, Store, Load, Gep };
enum class CallDir { None, Enter, Exit };

struct Node {
    NodeKind kind = NodeKind::Var;
    SvfId svfId = kNoSvfId;
    std::string name;           // IR/SVF name, or a generated fallback
    std::string function;       // demangled enclosing function
    std::string sourceName;     // variable name recovered from LLVM debug records, if available
    std::string sourceLocation; // original SVF location, including file information if available
    int line = 0;               // 0 when unavailable
};

struct Edge {
    NodeId src;
    NodeId dst;
    Label label;
    FieldId field = kNoField; // flattened struct offset; arrays follow SVF's abstraction
    CallSiteId callSite = kNoCallSite;
    CallDir dir = CallDir::None;
};

struct CallTarget {
    std::string function; // demangled callee
    std::vector<std::optional<NodeId>> formals;
    std::optional<NodeId> ret;
};

struct CallSite {
    SvfId svfId = kNoSvfId; // call ICFG node, not a PAG node
    std::string caller;
    std::string sourceLocation;
    int line = 0;
    bool isIndirect = false;
    bool isVirtual = false;
    // Null entries preserve argument positions for values excluded from the graph.
    std::vector<std::optional<NodeId>> actuals;
    std::optional<NodeId> actualRet;
    std::vector<CallTarget> targets; // empty for unresolved calls
};

// Owns its data: it remains usable after SVF and LLVM have been released.
// Clients can also construct a graph directly, without running SVF.
class ProgramGraph {
public:
    // Interns nodes with an SVF ID. Nodes with kNoSvfId are always distinct.
    NodeId addNode(const Node& node);
    std::optional<NodeId> findSvf(SvfId id) const;
    void addEdge(const Edge& edge) { edges_.push_back(edge); }
    CallSiteId addCallSite(const CallSite& site);
    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<Edge>& edges() const { return edges_; }
    const std::vector<CallSite>& callSites() const { return sites_; }
    void printSummary(std::ostream& out) const;
    void dumpDot(std::ostream& out) const;

private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<CallSite> sites_;
    std::unordered_map<SvfId, NodeId> bySvf_;
};

} // namespace ldc::frontend
