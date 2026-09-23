// Solver — points-to on LDGraph with k-limited call-string contexts (M2–M4, phase 2).
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
//   Ldcr  L_DCR_k = L_D ∩ C_k ∩ L_R, same edges. With one shared context, L_R's conditions
//         become state checks. The boxed ⟦ĉ_c⟧ of `a_i --store[p_i]--> r` in caller context C
//         opens a dispatch instance (c, C): the argument goes to field p_i of O *for (c, C)*.
//         `r#c --dispatch[t] ĉ_c--> this` in the same C closes it (the paper's ⟦č_c⟧ on
//         r → r#c): the receiver fact in `this` carries the tag (c, C), and
//         `this --load[p_i]-->` reads only that instance — DP-C1 (same site) and DP-C2 (same
//         context). Returns mirror this: `ret --store[ret]--> this` writes the instance of the
//         tag, the caller's `r --load[ret] ⟦č_c⟧--> x` in C reads (c, C). Tags live only in
//         `this` of the dispatched method: every other edge drops them.

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
    Ldcr,
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

    using CallString = std::vector<CallSiteId>; ///< most recent call site first
    /// (var, ctx) -> {(object, heap ctx)}, contexts as call strings; tags and offsets dropped.
    using ContextFacts =
        std::map<std::pair<NodeId, CallString>, std::set<std::pair<NodeId, CallString>>>;
    /// Context-sensitive facts of variables, for comparing analyses.
    ContextFacts contextFacts() const;

    std::size_t iterations() const { return iterations_; }
    std::size_t contextCount() const { return contexts_.size(); }
    /// Number of (function, context) pairs analysed.
    std::size_t methodContextCount() const;
    /// Functions reached in at least one context.
    std::size_t reachedFunctionCount() const;

    /// Virtual call edges found: (call site, callee), and with the caller's context.
    std::set<std::pair<CallSiteId, std::string>> virtualCallEdges() const;
    std::size_t virtualCallEdgeContextCount() const { return virtualCalls_.size(); }
    /// Receiver objects of unknown dynamic type that reached a virtual call (kcfa, ldc drop
    /// them: a source of unsoundness to watch in the evaluation).
    const std::set<NodeId>& untypedReceivers() const { return untypedReceivers_; }

private:
    using Ctx = CallString;
    using CtxId = std::uint32_t;
    /// Object with heap context, and an offset for interior pointers (&O.f, gep).
    struct Obj
    {
        NodeId node;
        CtxId ctx;
        FieldId offset = 0;
        /// Ldcr: the dispatch instance (call site, caller context) that passed this receiver
        /// to `this`; kNoCallSite elsewhere.
        CallSiteId tagSite = kNoCallSite;
        CtxId tagCtx = 0;
        bool operator<(const Obj& other) const
        {
            return std::tie(node, ctx, offset, tagSite, tagCtx) <
                   std::tie(other.node, other.ctx, other.offset, other.tagSite, other.tagCtx);
        }
    };
    using Var = std::pair<NodeId, CtxId>;                 ///< node in a context
    using Field = std::tuple<NodeId, CtxId, FieldId>;     ///< field of a heap object
    /// Ldcr: synthetic field (p_i / ret) of a heap object, per dispatch instance (site, ctx).
    using InstanceField = std::tuple<NodeId, CtxId, FieldId, CallSiteId, CtxId>;
    /// Field `field` of an object seen at `offset` (interior pointer).
    static FieldId shift(FieldId offset, FieldId field);
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
    ObjSet& fieldSet(const Obj& object, FieldId field);

    bool applyIntra(const Edge& edge, CtxId ctx);
    bool storeInstance(const Edge& edge, CtxId ctx);
    bool loadInstance(const Edge& edge, CtxId ctx);
    bool uses(Encoding encoding) const;
    bool applyCall(const Edge& edge, CtxId callerCtx);
    bool applyVirtualCall(CallSiteId site, CtxId callerCtx);

    const LDGraph& graph_;
    SolverOptions options_;

    std::vector<Ctx> contexts_;
    std::map<Ctx, CtxId> contextIds_;

    std::map<Var, ObjSet> pts_;
    std::map<Field, ObjSet> heap_;
    std::map<InstanceField, ObjSet> instanceHeap_;
    std::map<std::string, std::set<CtxId>> methodCtx_;

    /// Edges grouped by the function whose contexts drive them.
    std::map<std::string, std::vector<EdgeId>> intraByFunction_;
    std::map<std::string, std::vector<EdgeId>> callsByCaller_;
    std::map<std::string, std::vector<CallSiteId>> virtualSitesByCaller_;

    void recordCall(CallSiteId site, CtxId callerCtx, const std::string& callee);

    std::set<std::tuple<CallSiteId, CtxId, std::string>> virtualCalls_;
    std::set<NodeId> untypedReceivers_;
    std::size_t iterations_ = 0;
};

} // namespace ldc
