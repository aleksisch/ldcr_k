// LDGraph — the PAG the L_DC solver runs on (PLAN.md §2).
//
// Built from SVF's SVFIR by ldc::buildLDGraph (Builder.h). Unlike SVFIR it
// carries the L_D / C_k labels directly on its edges: fields on store/load,
// call sites and directions (ĉ / č) on call edges.

#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ldc
{

using NodeId = std::uint32_t;
using SvfId = std::uint32_t;

/// Call site index into LDGraph::callSites(), or kNoCallSite.
using CallSiteId = std::int32_t;
inline constexpr CallSiteId kNoCallSite = -1;

/// Field of store/load. Constant (flattened) field index, kNoField for
/// non-field edges, kAnyField for a variable offset (array-insensitive).
using FieldId = std::int32_t;
inline constexpr FieldId kNoField = -1;
inline constexpr FieldId kAnyField = -2;

enum class NodeKind : std::uint8_t
{
    Var, ///< SVF value variable (top-level pointer)
    Obj, ///< abstract object (allocation site)
};

/// Label above the edge (L_D alphabet).
enum class Label : std::uint8_t
{
    New,
    Assign,
    Store,
    Load,
};

/// Label below the edge (C_k alphabet): ĉ = Enter, č = Exit.
enum class CallDir : std::uint8_t
{
    None,
    Enter,
    Exit,
};

struct Node
{
    NodeKind kind;
    SvfId svfId;          ///< SVF node id, for traceability
    std::string name;     ///< variable / object name
    std::string function; ///< enclosing function (demangled), empty for objects
    int line = 0;         ///< source line (objects: allocation line), 0 if unknown
};

struct Edge
{
    NodeId src;
    NodeId dst;
    Label label;
    FieldId field = kNoField;
    CallSiteId callSite = kNoCallSite;
    CallDir dir = CallDir::None;
};

struct CallSite
{
    std::string caller; ///< demangled caller name
    int line = 0;       ///< source line of the call, 0 if unknown
    bool isVirtual = false;
};

class LDGraph
{
public:
    /// Returns the node for an SVF id, creating it on first use.
    NodeId nodeFor(SvfId svfId, NodeKind kind, const std::string& name,
                   const std::string& function, int line);
    std::optional<NodeId> findSvf(SvfId svfId) const
    {
        auto it = bySvf_.find(svfId);
        return it == bySvf_.end() ? std::nullopt : std::optional<NodeId>(it->second);
    }

    void addEdge(const Edge& edge) { edges_.push_back(edge); }
    CallSiteId addCallSite(const CallSite& callSite);

    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<Edge>& edges() const { return edges_; }
    const std::vector<CallSite>& callSites() const { return callSites_; }

    void printSummary(std::ostream& os) const;
    void dumpDot(std::ostream& os) const;

private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<CallSite> callSites_;
    std::unordered_map<SvfId, NodeId> bySvf_;
};

} // namespace ldc
