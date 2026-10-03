// Solver — points-to on LDGraph with k-limited call-string contexts.

#pragma once

#include "ldc/LDGraph.h"

#include <llvm/ADT/SparseBitVector.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ldc {

struct SolverOptions {
    unsigned k = 0;
    Mode mode = Mode::Lfc;
    /// Start every function in the empty context (as SVF's Andersen analyses every function),
    /// instead of only `main`. Used for the comparison with Andersen.
    bool allFunctionsReachable = false;
    /// Nodes analysed in context [] only (P3Ctx.h); empty: none.
    std::vector<bool> insensitive;
};

class Solver {
public:
    using CtxId = std::uint32_t;
    using CallString = std::vector<CallSiteId>; ///< most recent call site first
    /// (var, ctx) -> {(object, heap ctx)}, contexts as call strings; tags and offsets dropped.
    using ContextFacts =
        std::map<std::pair<NodeId, CallString>, std::set<std::pair<NodeId, CallString>>>;

    Solver(const LDGraph& graph, SolverOptions options);

    void solve();

    /// Objects (LDGraph node ids) that node `n` may point to, in any context.
    std::set<NodeId> pts(NodeId n) const;
    /// Context-sensitive facts of variables, for comparing analyses.
    ContextFacts contextFacts() const;

    /// Worklist pops.
    std::size_t iterations() const { return iterations_; }
    std::size_t contextCount() const { return contexts_.size(); }
    /// Number of (function, context) pairs analysed.
    std::size_t methodContextCount() const;

    /// Virtual call edges found: (call site, callee).
    const std::set<std::pair<CallSiteId, std::string>>& virtualCallEdges() const {
        return virtualCalls_;
    }
    /// Receiver objects of unknown dynamic type that reached a virtual call (kcfa, ldc, ldcr
    /// drop them: a source of unsoundness to watch).
    const std::set<NodeId>& untypedReceivers() const { return untypedReceivers_; }

private:
    using Ctx = CallString;

    /// Object with heap context, and an offset for interior pointers (&O.f, gep).
    struct Obj {
        NodeId node;
        CtxId ctx;
        FieldId offset = 0;
        /// Ldcr: the dispatch instance (call site, caller context) that passed this receiver
        /// to `this`; kNoCallSite elsewhere.
        CallSiteId tagSite = kNoCallSite;
        CtxId tagCtx = 0;
        bool operator<(const Obj& other) const {
            return std::tie(node, ctx, offset, tagSite, tagCtx) <
                   std::tie(other.node, other.ctx, other.offset, other.tagSite, other.tagCtx);
        }
    };
    using ObjSet = std::set<Obj>;

    /// Field `field` of `object` seen at `offset` (interior pointer). Synthetic fields stay
    /// synthetic, one set per subobject offset. Real fields beyond the object's field limit
    /// become `*` — as in SVF; otherwise a gep in a loop would grow offsets forever.
    FieldId shift(NodeId object, FieldId offset, FieldId field) const;
    /// The object without its dispatch-instance tag (plain flow).
    static Obj untagged(const Obj& object) { return Obj{object.node, object.ctx, object.offset}; }

    CtxId intern(const Ctx& ctx);
    CtxId push(CallSiteId site, CtxId ctx, unsigned limit);
    CtxId truncate(CtxId ctx, unsigned limit);
    /// Heap context of an object allocated in `ctx`: ⌈ctx⌉_{k-1}.
    CtxId heapContext(CtxId ctx);

    /// Context a node is read/written in, when its function runs in `ctx`
    /// (globals are context-insensitive).
    CtxId nodeCtx(NodeId n, CtxId ctx) const;
    /// Marks `function` reached in `ctx`; true if new.
    bool markReached(const std::string& function, CtxId ctx);
    /// Functions reached at the start (context []): the global scope and `main`, or every
    /// function with allFunctionsReachable.
    std::vector<std::string> entryFunctions() const;

    /// The dynamic types a receiver may have for dispatch: of the object, or of the member
    /// subobject an interior pointer points to. At offset `*` (a field-insensitive object) it
    /// may be the object or any of its members. Empty if not known.
    std::vector<TypeId> receiverTypes(const Obj& receiver) const;
    static bool hasType(const std::vector<TypeId>& types, TypeId type) {
        return std::find(types.begin(), types.end(), type) != types.end();
    }
    void recordCall(CallSiteId site, const std::string& callee) {
        virtualCalls_.insert({site, callee});
    }
    void recordUntyped(NodeId object) { untypedReceivers_.insert(object); }

    const LDGraph& graph_;
    SolverOptions options_;

    std::vector<Ctx> contexts_;
    std::map<Ctx, CtxId> contextIds_;
    std::map<std::string, std::set<CtxId>> methodCtx_;

    /// Edges grouped by the function whose contexts drive them.
    std::map<std::string, std::vector<EdgeId>> intraByFunction_;
    std::map<std::string, std::vector<EdgeId>> callsByCaller_;
    std::map<std::string, std::vector<CallSiteId>> virtualSitesByCaller_; ///< Kcfa only

    std::set<std::pair<CallSiteId, std::string>> virtualCalls_;
    std::set<NodeId> untypedReceivers_;
    std::size_t iterations_ = 0;

    using VarId = std::uint32_t;
    using CellId = std::uint32_t;
    using ObjId = std::uint32_t;
    using Bits = llvm::SparseBitVector<128>;

    /// Where the dispatch instance of a synthetic field access comes from (Ldcr only).
    enum class Instance : std::uint8_t {
        None,   ///< ordinary field (all other modes, and real fields)
        Edge,   ///< the edge's own call site and context (caller side, boxed ⟦ĉ⟧ / ⟦č⟧)
        Object, ///< the tag of the receiver object in `this` (callee side)
    };

    struct CopyEdge {
        VarId dst;
        bool gep = false;
        FieldId field = 0; ///< gep offset
    };
    struct FieldEdge { ///< store: value → base; load: base → dst
        VarId value;   ///< store: the stored value; load: the destination
        VarId base;
        FieldId field;
        Instance instance;
        CallSiteId site;
        CtxId ctx;
    };
    struct DispatchEdge { ///< r#c --dispatch[t] ĉ_c--> this (Ldc, Ldcr)
        VarId self;
        TypeId type;
        CallSiteId site;
        CtxId callerCtx;
        CtxId calleeCtx;
        const std::string* callee;
    };
    struct VirtualSite { ///< [I-VCall] at a virtual call site in a caller context (Kcfa)
        CallSiteId site;
        CtxId callerCtx;
    };

    struct VarInfo {
        NodeId node;
        CtxId ctx;
        Bits pts;
        Bits delta; ///< in pts, not yet propagated
        std::vector<CopyEdge> copies;
        std::vector<std::uint32_t> storesAsValue;
        std::vector<std::uint32_t> storesAsBase;
        std::vector<std::uint32_t> loadsAsBase;
        std::vector<std::uint32_t> dispatches;
        std::vector<std::uint32_t> virtualSites;
    };

    using CellKey = std::tuple<NodeId, CtxId, FieldId, CallSiteId, CtxId>;
    using ObjectKey = std::pair<NodeId, CtxId>;
    struct Cell {
        ObjectKey object;
        bool real; ///< a real field (read by load[*]), not a synthetic p_i / ret
        Bits objects;
        std::vector<VarId> readers;
    };

    // Objects.
    ObjId intern(const Obj& object);
    ObjId untaggedId(ObjId id) const { return untagged_[id]; }
    ObjId shifted(ObjId id, FieldId field);
    /// The same set with dispatch-instance tags dropped (plain flow).
    Bits plain(const Bits& objects) const;

    VarId var(NodeId node, CtxId ctx);
    void add(VarId v, ObjId object);
    void addBits(VarId v, const Bits& objects); ///< objects must be untagged or intended

    void reach(const std::string& function, CtxId ctx);
    void instantiate(const std::string& function, CtxId ctx);
    void addCopy(VarId src, CopyEdge edge);
    void copyInto(const CopyEdge& edge, const Bits& objects);

    /// The cell a field access touches for `object`; false if none (untagged `this`).
    bool cellKey(const Obj& object, const FieldEdge& edge, CellKey& key) const;
    CellId cell(const CellKey& key);
    void store(CellId cell, const Bits& values);
    void subscribe(CellId cell, VarId reader);
    void subscribeAllFields(const ObjectKey& object, VarId reader);
    void storeObject(const FieldEdge& edge, ObjId base, const Bits& values);
    void loadObject(const FieldEdge& edge, ObjId base);

    void dispatchObject(const DispatchEdge& edge, ObjId receiver);
    void virtualCallObject(const VirtualSite& site, ObjId receiver);

    void propagate(VarId v);

    std::vector<Obj> objs_;
    std::map<Obj, ObjId> objIds_;
    std::vector<ObjId> untagged_;
    std::unordered_map<std::uint64_t, ObjId> shiftCache_;
    bool anyTagged_ = false;

    std::vector<VarInfo> vars_;
    std::unordered_map<std::uint64_t, VarId> varIds_;
    std::unordered_map<NodeId, std::vector<VarId>> varsOfNode_;

    std::vector<FieldEdge> stores_;
    std::vector<FieldEdge> loads_;
    std::vector<DispatchEdge> dispatches_;
    std::vector<VirtualSite> virtualSites_;
    std::set<std::tuple<CallSiteId, CtxId, std::size_t>> wiredTargets_; ///< Kcfa

    std::vector<Cell> cells_;
    std::map<CellKey, CellId> cellIds_;
    std::map<ObjectKey, std::vector<VarId>> allFieldReaders_; ///< load[*] readers of an object
    std::map<ObjectKey, Bits> allFields_; ///< union of all real cells of an object (load[*])
    std::unordered_set<std::uint64_t> subscribed_; ///< (cell, reader)
    std::set<std::pair<ObjectKey, VarId>> subscribedAll_;

    std::deque<VarId> worklist_;
    std::deque<std::pair<std::string, CtxId>> toInstantiate_;
};

} // namespace ldc
