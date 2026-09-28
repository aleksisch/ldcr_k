#include "ldc/Solver.h"


namespace ldc
{

namespace
{

const std::string kGlobalScope; // functions of global nodes: ""
const std::string kEntry = "main";

} // namespace

Solver::Solver(const LDGraph& graph, SolverOptions options) : graph_(graph), options_(options)
{
    intern(Ctx{}); // CtxId 0 = []

    for (EdgeId e = 0; e < graph_.edges().size(); ++e)
    {
        const Edge& edge = graph_.edges()[e];
        if (!uses(edge.encoding))
            continue;
        if (edge.dir == CallDir::Enter || edge.dir == CallDir::Exit)
        {
            const CallSite& site = graph_.callSites()[static_cast<std::size_t>(edge.callSite)];
            callsByCaller_[site.caller].push_back(e);
            continue;
        }
        // An intra-procedural edge runs in the contexts of the function it belongs to:
        // that of its variable endpoint(s); an edge between globals only runs in [].
        const Node& dst = graph_.nodes()[edge.dst];
        const Node& src = graph_.nodes()[edge.src];
        const std::string& function =
            dst.kind == NodeKind::Var && !dst.function.empty() ? dst.function : src.function;
        intraByFunction_[function].push_back(e);
    }

    if (options_.mode == Mode::Kcfa)
        for (CallSiteId c = 0; c < static_cast<CallSiteId>(graph_.callSites().size()); ++c)
            if (graph_.callSites()[static_cast<std::size_t>(c)].isVirtual)
                virtualSitesByCaller_[graph_.callSites()[static_cast<std::size_t>(c)].caller]
                    .push_back(c);
}

bool Solver::uses(Encoding encoding) const
{
    switch (encoding)
    {
    case Encoding::Common:
        return true;
    case Encoding::Fc:
        return options_.mode == Mode::Lfc;
    case Encoding::D:
        return options_.mode == Mode::Ldc || options_.mode == Mode::Ldcr;
    }
    return false;
}

FieldId Solver::shift(NodeId object, FieldId offset, FieldId field) const
{
    // Synthetic fields belong to the receiver ⟨O, offset⟩: a member subobject has its own.
    if (isSyntheticField(field))
        return offset == 0 ? field : field - 1000 * (offset + 3);
    if (offset == kAnyField || field == kAnyField)
        return kAnyField;
    const FieldId result = offset + field;
    return result < graph_.nodes()[object].fieldLimit ? result : kAnyField;
}

Solver::CtxId Solver::intern(const Ctx& ctx)
{
    auto [it, inserted] = contextIds_.emplace(ctx, static_cast<CtxId>(contexts_.size()));
    if (inserted)
        contexts_.push_back(ctx);
    return it->second;
}

Solver::CtxId Solver::truncate(CtxId ctx, unsigned limit)
{
    const Ctx& full = contexts_[ctx];
    if (full.size() <= limit)
        return ctx;
    return intern(Ctx(full.begin(), full.begin() + limit));
}

Solver::CtxId Solver::push(CallSiteId site, CtxId ctx, unsigned limit)
{
    if (limit == 0)
        return 0;
    Ctx next;
    next.push_back(site);
    const Ctx& old = contexts_[ctx];
    next.insert(next.end(), old.begin(), old.end());
    if (next.size() > limit)
        next.resize(limit);
    return intern(next);
}

Solver::CtxId Solver::heapContext(CtxId ctx)
{
    return options_.k == 0 ? 0 : truncate(ctx, options_.k - 1);
}

Solver::CtxId Solver::nodeCtx(NodeId n, CtxId ctx) const
{
    return graph_.nodes()[n].function.empty() ? 0 : ctx;
}

bool Solver::markReached(const std::string& function, CtxId ctx)
{
    return methodCtx_[function].insert(ctx).second;
}

std::vector<std::string> Solver::entryFunctions() const
{
    std::vector<std::string> entries{kGlobalScope, kEntry};
    if (options_.allFunctionsReachable)
        for (const Node& node : graph_.nodes())
            if (!node.function.empty())
                entries.push_back(node.function);
    return entries;
}

std::vector<TypeId> Solver::receiverTypes(const Obj& receiver) const
{
    if (receiver.offset == kAnyField)
    {
        std::vector<TypeId> types = graph_.memberTypes(receiver.node);
        if (const TypeId own = graph_.nodes()[receiver.node].type; own != kUnknownType)
            types.push_back(own);
        return types;
    }
    const TypeId type = graph_.subobjectType(receiver.node, receiver.offset);
    return type == kUnknownType ? std::vector<TypeId>{} : std::vector<TypeId>{type};
}

void Solver::recordCall(CallSiteId site, CtxId callerCtx, const std::string& callee)
{
    virtualCalls_.insert({site, callerCtx, callee});
}

std::set<std::pair<CallSiteId, std::string>> Solver::virtualCallEdges() const
{
    std::set<std::pair<CallSiteId, std::string>> edges;
    for (const auto& [site, ctx, callee] : virtualCalls_)
        edges.insert({site, callee});
    return edges;
}

std::size_t Solver::reachedFunctionCount() const
{
    std::size_t count = 0;
    for (const auto& [function, contexts] : methodCtx_)
        count += !function.empty() && !contexts.empty();
    return count;
}

std::size_t Solver::methodContextCount() const
{
    std::size_t count = 0;
    for (const auto& entry : methodCtx_)
        count += entry.second.size();
    return count;
}


// ---- objects ---------------------------------------------------------------------------

Solver::ObjId Solver::intern(const Obj& object)
{
    if (auto it = objIds_.find(object); it != objIds_.end())
        return it->second;
    // A tagged object's untagged twin is interned first, so ids never need fixing.
    const ObjId plainId = object.tagSite != kNoCallSite ? intern(untagged(object)) : 0;
    const auto id = static_cast<ObjId>(objs_.size());
    objIds_.emplace(object, id);
    objs_.push_back(object);
    if (object.tagSite != kNoCallSite)
    {
        anyTagged_ = true;
        untagged_.push_back(plainId);
    }
    else
        untagged_.push_back(id);
    return id;
}

Solver::ObjId Solver::shifted(ObjId id, FieldId field)
{
    const std::uint64_t key =
        (static_cast<std::uint64_t>(id) << 32) | static_cast<std::uint32_t>(field);
    if (auto it = shiftCache_.find(key); it != shiftCache_.end())
        return it->second;
    const Obj object = objs_[id];
    const ObjId result = intern(Obj{object.node, object.ctx, shift(object.node, object.offset, field)});
    shiftCache_.emplace(key, result);
    return result;
}

Solver::Bits Solver::plain(const Bits& objects) const
{
    if (!anyTagged_)
        return objects;
    Bits result;
    for (ObjId id : objects)
        result.set(untagged_[id]);
    return result;
}

// ---- variables -------------------------------------------------------------------------

Solver::VarId Solver::var(NodeId node, CtxId ctx)
{
    const std::uint64_t key = (static_cast<std::uint64_t>(node) << 32) | ctx;
    auto [it, inserted] = varIds_.emplace(key, static_cast<VarId>(vars_.size()));
    if (inserted)
    {
        VarInfo info;
        info.node = node;
        info.ctx = ctx;
        vars_.push_back(std::move(info));
        varsOfNode_[node].push_back(it->second);
    }
    return it->second;
}

void Solver::add(VarId v, ObjId object)
{
    VarInfo& info = vars_[v];
    if (!info.pts.test_and_set(object))
        return;
    if (info.delta.empty())
        worklist_.push_back(v);
    info.delta.set(object);
}

void Solver::addBits(VarId v, const Bits& objects)
{
    VarInfo& info = vars_[v];
    Bits fresh;
    fresh.intersectWithComplement(objects, info.pts); // objects \ pts, without copying objects
    if (fresh.empty())
        return;
    info.pts |= fresh;
    if (info.delta.empty())
        worklist_.push_back(v);
    info.delta |= fresh;
}

void Solver::copyInto(const CopyEdge& edge, const Bits& objects)
{
    if (!edge.gep)
    {
        if (anyTagged_)
            addBits(edge.dst, plain(objects));
        else
            addBits(edge.dst, objects);
        return;
    }
    Bits moved;
    for (ObjId id : objects)
        moved.set(shifted(untagged_[id], edge.field));
    addBits(edge.dst, moved);
}

void Solver::addCopy(VarId src, CopyEdge edge)
{
    vars_[src].copies.push_back(edge);
    const Bits current = vars_[src].pts;
    copyInto(edge, current);
}

// ---- heap ------------------------------------------------------------------------------

namespace
{
constexpr NodeId kInstanceCellNode = 0xffffffffu; ///< owner of Ldcr's per-instance cells
} // namespace

bool Solver::cellKey(const Obj& object, const FieldEdge& edge, CellKey& key) const
{
    const FieldId field = shift(object.node, object.offset, edge.field);
    switch (edge.instance)
    {
    case Instance::None:
        key = {object.node, object.ctx, field, kNoCallSite, 0};
        return true;
    // A dispatch instance's cells do not depend on the receiver object: every receiver tagged
    // (c, C) was dispatched from r#c in C, so it is in pts(r) in C and its per-object cell would
    // hold exactly the arguments stored at (c, C); the result, read in C over pts(r), is the
    // union either way. Keying by (field, c, C) alone gives the same facts and one cell per
    // instance, not per object.
    case Instance::Edge:
        key = {kInstanceCellNode, 0, edge.field, edge.site, edge.ctx};
        return true;
    case Instance::Object:
        if (object.tagSite == kNoCallSite)
            return false; // `this` reached by a direct call: its arguments flow by assign edges
        key = {kInstanceCellNode, 0, edge.field, object.tagSite, object.tagCtx};
        return true;
    }
    return false;
}

Solver::CellId Solver::cell(const CellKey& key)
{
    auto [it, inserted] = cellIds_.emplace(key, static_cast<CellId>(cells_.size()));
    if (inserted)
    {
        const ObjectKey object{std::get<0>(key), std::get<1>(key)};
        const bool real = !isSyntheticField(std::get<2>(key));
        cells_.push_back(Cell{object, real, {}, {}});
    }
    return it->second;
}

void Solver::store(CellId c, const Bits& values)
{
    Bits fresh;
    if (anyTagged_)
        fresh.intersectWithComplement(plain(values), cells_[c].objects);
    else
        fresh.intersectWithComplement(values, cells_[c].objects);
    if (fresh.empty())
        return;
    cells_[c].objects |= fresh;
    // addBits only touches variables and the worklist, so the reader lists stay valid.
    for (VarId reader : cells_[c].readers)
        addBits(reader, fresh);
    if (!cells_[c].real)
        return;
    // load[*] readers see the union of all real cells of the object: notify them only of
    // objects new to that union (otherwise an object arrives once per cell holding it).
    Bits& all = allFields_[cells_[c].object];
    Bits freshForAll;
    freshForAll.intersectWithComplement(fresh, all);
    if (freshForAll.empty())
        return;
    all |= freshForAll;
    if (auto it = allFieldReaders_.find(cells_[c].object); it != allFieldReaders_.end())
        for (VarId reader : it->second)
            addBits(reader, freshForAll);
}

void Solver::subscribe(CellId c, VarId reader)
{
    if (!subscribed_.insert((static_cast<std::uint64_t>(c) << 32) | reader).second)
        return;
    cells_[c].readers.push_back(reader);
    const Bits current = cells_[c].objects;
    addBits(reader, current);
}

/// load[*]: every real field of the object, now and later.
void Solver::subscribeAllFields(const ObjectKey& object, VarId reader)
{
    if (!subscribedAll_.insert({object, reader}).second)
        return;
    allFieldReaders_[object].push_back(reader);
    if (auto it = allFields_.find(object); it != allFields_.end())
    {
        const Bits current = it->second;
        addBits(reader, current);
    }
}

void Solver::storeObject(const FieldEdge& edge, ObjId base, const Bits& values)
{
    CellKey key;
    if (cellKey(objs_[base], edge, key))
        store(cell(key), values);
}

void Solver::loadObject(const FieldEdge& edge, ObjId baseId)
{
    const VarId dst = edge.value;
    const Obj base = objs_[baseId];
    if (edge.instance != Instance::None)
    {
        CellKey key;
        if (cellKey(base, edge, key))
            subscribe(cell(key), dst);
        return;
    }
    const FieldId field = shift(base.node, base.offset, edge.field);
    auto at = [&](FieldId f) { return CellKey{base.node, base.ctx, f, kNoCallSite, 0}; };
    if (isSyntheticField(field))
        subscribe(cell(at(field)), dst);
    else if (field == kAnyField)
        subscribeAllFields({base.node, base.ctx}, dst);
    else
    {
        subscribe(cell(at(field)), dst);
        subscribe(cell(at(kAnyField)), dst);
    }
}

// ---- virtual calls ---------------------------------------------------------------------

void Solver::dispatchObject(const DispatchEdge& edge, ObjId receiverId)
{
    const Obj receiver = objs_[receiverId];
    const std::vector<TypeId> types = receiverTypes(receiver);
    if (types.empty())
        recordUntyped(receiver.node);
    if (!hasType(types, edge.type))
        return;
    recordCall(edge.site, edge.callerCtx, *edge.callee);
    reach(*edge.callee, edge.calleeCtx);
    Obj passed = untagged(receiver);
    if (options_.mode == Mode::Ldcr) // closes the instance (c, callerCtx): tag it
    {
        passed.tagSite = edge.site;
        passed.tagCtx = edge.callerCtx;
    }
    add(edge.self, intern(passed));
}

void Solver::virtualCallObject(const VirtualSite& vsite, ObjId receiverId)
{
    const Obj receiver = objs_[receiverId];
    const CallSite& site = graph_.callSites()[static_cast<std::size_t>(vsite.site)];
    const std::vector<TypeId> types = receiverTypes(receiver);
    if (types.empty())
    {
        recordUntyped(receiver.node);
        return;
    }
    const CtxId calleeCtx = push(vsite.site, vsite.callerCtx, options_.k);
    for (std::size_t t = 0; t < site.targets.size(); ++t)
    {
        const VirtualTarget& target = site.targets[t];
        if (target.formals.empty() ||
            std::none_of(types.begin(), types.end(), [&](TypeId t) { return hasType(target.types, t); }))
            continue;
        const std::string& callee = graph_.nodes()[target.formals[0]].function;
        recordCall(vsite.site, vsite.callerCtx, callee);
        reach(callee, calleeCtx);
        // [I-VCall]: the receiver object goes to `this` of its own target only ...
        add(var(target.formals[0], calleeCtx), untaggedId(receiverId));
        if (!wiredTargets_.insert({vsite.site, vsite.callerCtx, t}).second)
            continue;
        // ... the other actuals (and the result) to every target it dispatches to.
        for (std::size_t i = 1; i < target.formals.size() && i < site.actuals.size(); ++i)
            if (site.actuals[i])
                addCopy(var(*site.actuals[i], nodeCtx(*site.actuals[i], vsite.callerCtx)),
                        CopyEdge{var(target.formals[i], calleeCtx), false, 0});
        if (target.ret && site.actualRet)
            addCopy(var(*target.ret, calleeCtx),
                    CopyEdge{var(*site.actualRet, nodeCtx(*site.actualRet, vsite.callerCtx)), false, 0});
    }
}

// ---- functions in contexts -------------------------------------------------------------

void Solver::reach(const std::string& function, CtxId ctx)
{
    if (markReached(function, ctx))
        toInstantiate_.push_back({function, ctx});
}

void Solver::instantiate(const std::string& function, CtxId ctx)
{
    const bool ldcr = options_.mode == Mode::Ldcr;

    if (auto it = intraByFunction_.find(function); it != intraByFunction_.end())
        for (EdgeId e : it->second)
        {
            const Edge& edge = graph_.edges()[e];
            const VarId src = var(edge.src, nodeCtx(edge.src, ctx));
            const VarId dst = var(edge.dst, nodeCtx(edge.dst, ctx));
            switch (edge.label)
            {
            case Label::New:
                add(dst, intern(Obj{edge.src, heapContext(nodeCtx(edge.dst, ctx))}));
                break;
            case Label::Assign:
                addCopy(src, CopyEdge{dst, false, 0});
                break;
            case Label::Gep:
                addCopy(src, CopyEdge{dst, true, edge.field});
                break;
            case Label::Store: // src --store[f]--> dst
            {
                Instance instance = Instance::None;
                if (ldcr && isSyntheticField(edge.field))
                    instance = edge.dir == CallDir::BoxEnter ? Instance::Edge : Instance::Object;
                const FieldEdge store{src, dst, edge.field, instance, edge.callSite, ctx};
                const auto id = static_cast<std::uint32_t>(stores_.size());
                stores_.push_back(store);
                vars_[src].storesAsValue.push_back(id);
                vars_[dst].storesAsBase.push_back(id);
                const Bits bases = vars_[dst].pts;
                const Bits values = vars_[src].pts;
                for (ObjId base : bases)
                    storeObject(store, base, values);
                break;
            }
            case Label::Load: // src --load[f]--> dst
            {
                Instance instance = Instance::None;
                if (ldcr && isSyntheticField(edge.field))
                    instance = edge.dir == CallDir::BoxExit ? Instance::Edge : Instance::Object;
                const FieldEdge load{dst, src, edge.field, instance, edge.callSite, ctx};
                const auto id = static_cast<std::uint32_t>(loads_.size());
                loads_.push_back(load);
                vars_[src].loadsAsBase.push_back(id);
                const Bits bases = vars_[src].pts;
                for (ObjId base : bases)
                    loadObject(load, base);
                break;
            }
            case Label::Dispatch: // always a call edge
                break;
            }
        }

    if (auto it = callsByCaller_.find(function); it != callsByCaller_.end())
        for (EdgeId e : it->second)
        {
            const Edge& edge = graph_.edges()[e];
            const CtxId calleeCtx = push(edge.callSite, ctx, options_.k);
            const std::string& callee = graph_.nodes()[edge.dst].function;
            if (edge.label == Label::Dispatch)
            {
                const DispatchEdge dispatch{var(edge.dst, calleeCtx), edge.type, edge.callSite,
                                            ctx, calleeCtx, &callee};
                const VarId receiver = var(edge.src, nodeCtx(edge.src, ctx));
                const auto id = static_cast<std::uint32_t>(dispatches_.size());
                dispatches_.push_back(dispatch);
                vars_[receiver].dispatches.push_back(id);
                const Bits receivers = vars_[receiver].pts;
                for (ObjId object : receivers)
                    dispatchObject(dispatch, object);
            }
            else if (edge.dir == CallDir::Enter) // actual (caller) → formal (callee)
            {
                if (graph_.callSites()[static_cast<std::size_t>(edge.callSite)].isVirtual)
                    recordCall(edge.callSite, ctx, callee);
                reach(callee, calleeCtx);
                addCopy(var(edge.src, nodeCtx(edge.src, ctx)),
                        CopyEdge{var(edge.dst, nodeCtx(edge.dst, calleeCtx)), false, 0});
            }
            else // formal return (callee) → actual return (caller)
            {
                addCopy(var(edge.src, nodeCtx(edge.src, calleeCtx)),
                        CopyEdge{var(edge.dst, nodeCtx(edge.dst, ctx)), false, 0});
            }
        }

    if (auto it = virtualSitesByCaller_.find(function); it != virtualSitesByCaller_.end())
        for (CallSiteId c : it->second)
        {
            const CallSite& site = graph_.callSites()[static_cast<std::size_t>(c)];
            if (site.actuals.empty() || !site.actuals[0])
                continue;
            const VarId receiver = var(*site.actuals[0], nodeCtx(*site.actuals[0], ctx));
            const VirtualSite vsite{c, ctx};
            const auto id = static_cast<std::uint32_t>(virtualSites_.size());
            virtualSites_.push_back(vsite);
            vars_[receiver].virtualSites.push_back(id);
            const Bits receivers = vars_[receiver].pts;
            for (ObjId object : receivers)
                virtualCallObject(vsite, object);
        }
}

// ---- propagation -----------------------------------------------------------------------

/// Pushes the new objects of v through every constraint v takes part in. Lists are read by
/// index: handling an object may create variables (vars_ grows) or constraints.
void Solver::propagate(VarId v)
{
    Bits delta;
    std::swap(delta, vars_[v].delta);
    if (delta.empty())
        return;

    for (std::size_t i = 0; i < vars_[v].copies.size(); ++i)
    {
        const CopyEdge edge = vars_[v].copies[i];
        copyInto(edge, delta);
    }
    for (std::size_t i = 0; i < vars_[v].storesAsValue.size(); ++i)
    {
        const FieldEdge store = stores_[vars_[v].storesAsValue[i]];
        const Bits bases = vars_[store.base].pts;
        for (ObjId base : bases)
            storeObject(store, base, delta);
    }
    for (std::size_t i = 0; i < vars_[v].storesAsBase.size(); ++i)
    {
        const FieldEdge store = stores_[vars_[v].storesAsBase[i]];
        const Bits values = vars_[store.value].pts;
        for (ObjId base : delta)
            storeObject(store, base, values);
    }
    for (std::size_t i = 0; i < vars_[v].loadsAsBase.size(); ++i)
    {
        const FieldEdge load = loads_[vars_[v].loadsAsBase[i]];
        for (ObjId base : delta)
            loadObject(load, base);
    }
    for (std::size_t i = 0; i < vars_[v].dispatches.size(); ++i)
    {
        const DispatchEdge dispatch = dispatches_[vars_[v].dispatches[i]];
        for (ObjId object : delta)
            dispatchObject(dispatch, object);
    }
    for (std::size_t i = 0; i < vars_[v].virtualSites.size(); ++i)
    {
        const VirtualSite vsite = virtualSites_[vars_[v].virtualSites[i]];
        for (ObjId object : delta)
            virtualCallObject(vsite, object);
    }
}

void Solver::solve()
{
    for (const std::string& function : entryFunctions())
        reach(function, 0);
    while (!toInstantiate_.empty() || !worklist_.empty())
    {
        if (!toInstantiate_.empty())
        {
            const auto [function, ctx] = toInstantiate_.front();
            toInstantiate_.pop_front();
            instantiate(function, ctx);
            continue;
        }
        const VarId v = worklist_.front();
        worklist_.pop_front();
        ++iterations_;
        propagate(v);
    }
}
// ---- results ---------------------------------------------------------------------------

std::set<NodeId> Solver::pts(NodeId n) const
{
    std::set<NodeId> result;
    if (auto it = varsOfNode_.find(n); it != varsOfNode_.end())
        for (VarId v : it->second)
            for (ObjId id : vars_[v].pts)
                result.insert(objs_[id].node);
    return result;
}

Solver::ContextFacts Solver::contextFacts() const
{
    ContextFacts facts;
    for (const VarInfo& info : vars_)
    {
        if (graph_.nodes()[info.node].kind != NodeKind::Var || info.pts.empty())
            continue;
        auto& into = facts[{info.node, contexts_[info.ctx]}];
        for (ObjId id : info.pts)
            into.insert({objs_[id].node, contexts_[objs_[id].ctx]});
    }
    return facts;
}

} // namespace ldc
