#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ldc::frontend {

struct NodeId {
    std::uint32_t value{};
    friend bool operator==(NodeId lhs, NodeId rhs);
    friend bool operator!=(NodeId lhs, NodeId rhs);
};

struct SvfId {
    std::uint32_t value{};
    friend bool operator==(SvfId lhs, SvfId rhs);
    friend bool operator!=(SvfId lhs, SvfId rhs);
};

struct FieldOffset {
    std::int32_t value{};
    friend bool operator==(FieldOffset lhs, FieldOffset rhs);
    friend bool operator!=(FieldOffset lhs, FieldOffset rhs);
};

struct CallSiteId {
    std::int32_t value{};
    friend bool operator==(CallSiteId lhs, CallSiteId rhs);
    friend bool operator!=(CallSiteId lhs, CallSiteId rhs);
};

} // namespace ldc::frontend

template <> struct std::hash<ldc::frontend::SvfId> {
    std::size_t operator()(ldc::frontend::SvfId id) const noexcept;
};

namespace ldc::frontend {

// Unknown field offset; an empty optional means the edge has no field.
inline constexpr FieldOffset kAnyField{-2};

enum class NodeKind { Var, Obj };
enum class Label { New, Assign, Store, Load, Gep };
enum class CallDir { None, Enter, Exit };

struct Node {
    NodeKind kind = NodeKind::Var;
    std::optional<SvfId> svfId;
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
    std::optional<FieldOffset> field; // flattened struct offset; arrays follow SVF's abstraction
    std::optional<CallSiteId> callSite;
    CallDir dir = CallDir::None;
};

struct CallTarget {
    std::string function; // demangled callee
    std::vector<std::optional<NodeId>> formals;
    std::optional<NodeId> ret;
};

struct CallFlags {
    std::uint32_t isIndirect : 1;
    std::uint32_t isVirtual : 1;
};

struct CallSite {
    std::optional<SvfId> svfId; // call ICFG node, not a PAG node
    std::string caller;
    std::string sourceLocation;
    int line = 0;
    CallFlags flags{};
    // Null entries preserve argument positions for values excluded from the graph.
    std::vector<std::optional<NodeId>> actuals;
    std::optional<NodeId> actualRet;
    std::vector<CallTarget> targets; // empty for unresolved calls
};

// Owns its data: it remains usable after SVF and LLVM have been released.
// Clients can also construct a graph directly, without running SVF.
class SimplifiedPAG {
public:
    // Interns nodes with an SVF ID. Nodes without an SVF ID are always distinct.
    NodeId addNode(const Node& node);
    std::optional<NodeId> findSvf(SvfId id) const;
    void addEdge(const Edge& edge);
    CallSiteId addCallSite(const CallSite& site);
    const std::vector<Node>& nodes() const;
    const std::vector<Edge>& edges() const;
    const std::vector<CallSite>& callSites() const;
    void printSummary(std::ostream& out) const;
    void dumpDot(std::ostream& out) const;

private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<CallSite> sites_;
    std::unordered_map<SvfId, NodeId> bySvf_;
};

} // namespace ldc::frontend
