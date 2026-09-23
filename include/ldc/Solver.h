// Solver — points-to on LDGraph with k-limited call-string contexts (M2, M3, M4).
//
// Contexts follow PLAN.md §2.3 (C_k as one shared context, kCFA's discipline):
//   entering a call at c:  ctx  →  ⌈c :: ctx⌉_k
//   returning at c:        the caller's ctx is the one the call was entered from
//   allocation:            the object gets heap context ⌈ctx⌉_{k-1}
// Facts are tagged with contexts, ⟨node, ctx⟩ — the finite-state product of L_F with
// C_k, built lazily (only reachable contexts). Globals live in the empty context.
//
// Fields: store[f] into b and load[f] out of b' meet through the per-object field heap,
// i.e. exactly when b and b' share an object (the alias U-turn of L_F).
//
// Modes (which encoding of virtual calls is used, see Encoding in LDGraph.h):
//   Lfc   baseline L_FC_k: the plain assign(ĉ/č) edges wired from the Andersen call graph
//         (paper Fig. 2, [P-VCall]).
//   Kcfa  oracle, paper Fig. 1: no edges; a virtual call dispatches each receiver object
//         ⟨O, h⟩ separately, by O's dynamic type ([I-VCall]): O goes to `this` of its own
//         target only, the other actuals to every target it dispatches to.
//   Ldc   L_DC_k = L_D ∩ C_k on the paper's Fig. 6 edges. dispatch[t] ĉ passes only objects
//         of type t (L_D). Arguments and the result go through synthetic fields of the
//         receiver object: store[p_i] in the caller, load[p_i] from `this` in the callee.
//         The boxed ⟦ĉ⟧ / ⟦č⟧ are ε for C_k, so these are intra-procedural edges.
//         Precision loss vs kCFA (paper Eq. 13, 15): the field p_i of ⟨O, h⟩ is shared by
//         all call sites and all caller contexts in which O is the receiver.

#pragma once

#include "ldc/LDGraph.h"

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ldc
{

enum class Mode
{
    Lfc,
    Kcfa,
    Ldc,
};

struct SolverOptions
{
    unsigned k = 0;
    Mode mode = Mode::Lfc;
    /// Start every function in the empty context (as SVF's Andersen analyses every function),
    /// instead of only `main`. Used for the comparison with Andersen.
    bool allFunctionsReachable = false;
};

class Solver
{
public:
    Solver(const LDGraph& graph, SolverOptions options);

    void solve();

    /// Objects (LDGraph node ids) that node `n` may point to, in any context.
    std::set<NodeId> pts(NodeId n) const;

    std::size_t iterations() const { return iterations_; }
    std::size_t contextCount() const { return contexts_.size(); }
    /// Number of (function, context) pairs analysed.
    std::size_t methodContextCount() const;

private:
    using Ctx = std::vector<CallSiteId>; ///< most recent call site first
    using CtxId = std::uint32_t;
    using Obj = std::pair<NodeId, CtxId>;                 ///< object with heap context
    using Var = std::pair<NodeId, CtxId>;                 ///< node in a context
    using Field = std::tuple<NodeId, CtxId, FieldId>;     ///< field of a heap object
    using ObjSet = std::set<Obj>;

    CtxId intern(const Ctx& ctx);
    CtxId push(CallSiteId site, CtxId ctx, unsigned limit);
    CtxId truncate(CtxId ctx, unsigned limit);

    /// Context a node is read/written in, when its function runs in `ctx`
    /// (globals are context-insensitive).
    CtxId nodeCtx(NodeId n, CtxId ctx) const;
    bool reach(const std::string& function, CtxId ctx);

    bool addAll(ObjSet& into, const ObjSet& from);
    ObjSet readField(const Obj& object, FieldId field) const;

    bool applyIntra(const Edge& edge, CtxId ctx);
    bool uses(Encoding encoding) const;
    bool applyCall(const Edge& edge, CtxId callerCtx);
    bool applyVirtualCall(CallSiteId site, CtxId callerCtx);

    const LDGraph& graph_;
    SolverOptions options_;

    std::vector<Ctx> contexts_;
    std::map<Ctx, CtxId> contextIds_;

    std::map<Var, ObjSet> pts_;
    std::map<Field, ObjSet> heap_;
    std::map<std::string, std::set<CtxId>> methodCtx_;

    /// Edges grouped by the function whose contexts drive them.
    std::map<std::string, std::vector<EdgeId>> intraByFunction_;
    std::map<std::string, std::vector<EdgeId>> callsByCaller_;
    std::map<std::string, std::vector<CallSiteId>> virtualSitesByCaller_;

    std::size_t iterations_ = 0;
};

} // namespace ldc
