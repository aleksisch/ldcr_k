// LDGraph — the PAG the L_DC solver runs on (PLAN.md §2).
//
// Built from SVF's SVFIR by ldc::buildLDGraph (Builder.h). Unlike SVFIR it
// carries the L_D / C_k labels directly on its edges: fields on store/load,
// types on dispatch, call sites and directions (ĉ / č) on call edges.

#pragma once

#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ldc
{

using NodeId = std::uint32_t;
using EdgeId = std::uint32_t;
using SvfId = std::uint32_t;
/// svfId of nodes with no SVF counterpart (synthetic objects of construction sites).
inline constexpr SvfId kNoSvfId = 0xffffffffu;

/// Call site index into LDGraph::callSites(), or kNoCallSite.
using CallSiteId = std::int32_t;
inline constexpr CallSiteId kNoCallSite = -1;

/// Field of store/load. Constant (flattened) field index, kNoField for
/// non-field edges, kAnyField for a variable offset (array-insensitive).
using FieldId = std::int32_t;
inline constexpr FieldId kNoField = -1;
inline constexpr FieldId kAnyField = -2;

/// Synthetic fields of a receiver object (paper [C-Param], [C-Ret]): the return value and
/// the i-th non-this parameter (i >= 1). The paper uses offsets 0 and i; ours are kept
/// apart from real field offsets (field 0 of a C++ object is its vptr).
inline constexpr FieldId kRetField = -10;
inline constexpr FieldId paramField(int i) { return kRetField - i; }
inline constexpr bool isSyntheticField(FieldId f) { return f <= kRetField; }
inline constexpr FieldId kDefaultFieldLimit = 512;

/// Dynamic type (a class with a vtable), index into LDGraph::types(); kUnknownType for
/// objects without one (plain structs, globals, ...).
using TypeId = std::int32_t;
inline constexpr TypeId kUnknownType = -1;

enum class NodeKind : std::uint8_t
{
    Var,      ///< SVF value variable (top-level pointer)
    Obj,      ///< abstract object (allocation site)
    RecvCopy, ///< per-call-site receiver copy r#c
};

/// Label above the edge (L_D alphabet).
enum class Label : std::uint8_t
{
    New,
    Assign,
    Store,
    Load,
    Dispatch,
    Gep, ///< q = &p->f used as a value (C++ interior pointer): q points to ⟨O, off + f⟩
};

/// Label below the edge. ĉ = Enter, č = Exit (C_k alphabet). The boxed ⟦ĉ⟧ / ⟦č⟧ mark the
/// start / end of a dispatch at a virtual call (paper Fig. 6): ε for C_k, used by L_R (phase 2).
enum class CallDir : std::uint8_t
{
    None,
    Enter,
    Exit,
    BoxEnter,
    BoxExit,
};

/// Which analyses an edge belongs to. Virtual calls have two encodings:
/// Fc — plain assign(ĉ/č) edges to the Andersen targets (paper Fig. 2, [P-VCall]);
/// D  — receiver fields, r#c and dispatch[t] edges to the CHA targets (paper Fig. 6, [C-VCall]).
enum class Encoding : std::uint8_t
{
    Common,
    Fc,
    D,
};

struct Node
{
    NodeKind kind;
    SvfId svfId;          ///< SVF node id, for traceability
    std::string name;     ///< variable / object name
    std::string function; ///< enclosing function (demangled), empty for objects
    int line = 0;         ///< source line (objects: allocation line), 0 if unknown
    TypeId type = kUnknownType; ///< objects: dynamic type, known at allocation
    /// Objects: number of (flattened) fields as SVF has it; offsets at or beyond it become
    /// `*`. 0: SVF made the object field-insensitive. At most SVF's MaxFieldLimit (512).
    FieldId fieldLimit = kDefaultFieldLimit;
};

struct Type
{
    std::string name; ///< class name, e.g. "B"
    NodeId vtable;    ///< its vtable object
};

struct Edge
{
    NodeId src;
    NodeId dst;
    Label label;
    FieldId field = kNoField;
    TypeId type = kUnknownType; ///< dispatch[t]
    CallSiteId callSite = kNoCallSite;
    CallDir dir = CallDir::None;
    Encoding encoding = Encoding::Common;
};

/// One possible target of a virtual call site (CHA: the method in the call's vtable slot of
/// some class): the method, the vtables that dispatch to it here, and its formals.
struct VirtualTarget
{
    std::string callee;            ///< demangled method name
    std::vector<NodeId> vtables;   ///< vtable objects whose slot at this site is `callee`
    std::vector<TypeId> types;     ///< the dynamic types that dispatch to `callee` here
    std::vector<NodeId> formals;   ///< formals[0] is `this`
    std::optional<NodeId> ret;     ///< formal return, if any
};

struct CallSite
{
    std::string caller; ///< demangled caller name
    int line = 0;       ///< source line of the call, 0 if unknown
    bool isVirtual = false;

    // Filled for virtual call sites only (used by the kCFA oracle and by L_D).
    std::vector<std::optional<NodeId>> actuals; ///< actuals[0] is the receiver
    std::optional<NodeId> actualRet;
    std::vector<VirtualTarget> targets;
};

class LDGraph
{
public:
    /// Returns the node for an SVF id, creating it on first use.
    NodeId nodeFor(SvfId svfId, NodeKind kind, const std::string& name,
                   const std::string& function, int line);
    /// Adds a node with no SVF counterpart of its own (r#c).
    NodeId addNode(const Node& node);
    std::optional<NodeId> findSvf(SvfId svfId) const
    {
        auto it = bySvf_.find(svfId);
        return it == bySvf_.end() ? std::nullopt : std::optional<NodeId>(it->second);
    }

    void addEdge(const Edge& edge) { edges_.push_back(edge); }
    CallSiteId addCallSite(const CallSite& callSite);
    CallSite& callSite(CallSiteId id) { return callSites_[static_cast<std::size_t>(id)]; }
    Node& node(NodeId id) { return nodes_[id]; }

    /// The type of a class, creating it on first use.
    TypeId typeFor(const std::string& className, NodeId vtable);
    const std::vector<Type>& types() const { return types_; }

    /// Dynamic type of the member subobject at `offset` of `object` (e.g. a polymorphic
    /// member), kUnknownType if none. Offset 0 is the object itself (Node::type), or its first
    /// member if the object's own class is not polymorphic.
    TypeId subobjectType(NodeId object, FieldId offset) const;
    void setSubobjectType(NodeId object, FieldId offset, TypeId type);
    /// The dynamic types of all member subobjects of `object` (any offset).
    const std::vector<TypeId>& memberTypes(NodeId object) const;

    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<Edge>& edges() const { return edges_; }
    const std::vector<CallSite>& callSites() const { return callSites_; }

    void printSummary(std::ostream& os) const;
    void dumpDot(std::ostream& os) const;

private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<CallSite> callSites_;
    std::vector<Type> types_;
    std::map<std::pair<NodeId, FieldId>, TypeId> subobjectTypes_;
    std::map<NodeId, std::vector<TypeId>> memberTypes_;
    std::unordered_map<SvfId, NodeId> bySvf_;
};

} // namespace ldc
