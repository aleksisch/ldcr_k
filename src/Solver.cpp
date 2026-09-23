#include "ldc/Solver.h"

#include <algorithm>

namespace ldc
{

namespace
{

const std::string kGlobalScope; // functions of global nodes: ""
const std::string kEntry = "main";

} // namespace

Solver::Solver(const LDGraph& graph, SolverOptions options) : graph_(graph), options_(options)
{
    intern({}); // CtxId 0 = []

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
        return options_.mode == Mode::Ldc;
    }
    return false;
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

Solver::CtxId Solver::nodeCtx(NodeId n, CtxId ctx) const
{
    return graph_.nodes()[n].function.empty() ? 0 : ctx;
}

bool Solver::reach(const std::string& function, CtxId ctx)
{
    return methodCtx_[function].insert(ctx).second;
}

bool Solver::addAll(ObjSet& into, const ObjSet& from)
{
    const std::size_t before = into.size();
    into.insert(from.begin(), from.end());
    return into.size() != before;
}

/// Field read with array-insensitive `*`: load[f] also sees store[*], and load[*]
/// sees every real field of the object. Synthetic fields (p_i, ret) are separate from both.
Solver::ObjSet Solver::readField(const Obj& object, FieldId field) const
{
    ObjSet result;
    if (field == kAnyField)
    {
        for (auto it = heap_.lower_bound({object.first, object.second, kAnyField});
             it != heap_.end() && std::get<0>(it->first) == object.first &&
             std::get<1>(it->first) == object.second;
             ++it)
            result.insert(it->second.begin(), it->second.end());
        return result;
    }
    if (isSyntheticField(field))
    {
        auto it = heap_.find({object.first, object.second, field});
        return it != heap_.end() ? it->second : result;
    }
    for (FieldId f : {field, kAnyField})
    {
        auto it = heap_.find({object.first, object.second, f});
        if (it != heap_.end())
            result.insert(it->second.begin(), it->second.end());
    }
    return result;
}

bool Solver::applyIntra(const Edge& edge, CtxId ctx)
{
    const Var src{edge.src, nodeCtx(edge.src, ctx)};
    const Var dst{edge.dst, nodeCtx(edge.dst, ctx)};
    bool changed = false;
    switch (edge.label)
    {
    case Label::New:
    {
        const CtxId heapCtx = options_.k == 0 ? 0 : truncate(dst.second, options_.k - 1);
        changed |= pts_[dst].insert({edge.src, heapCtx}).second;
        break;
    }
    case Label::Assign:
        changed |= addAll(pts_[dst], pts_[src]);
        break;
    case Label::Dispatch: // always a call edge (ĉ)
        break;
    case Label::Store: // src --store[f]--> base
    {
        const ObjSet bases = pts_[dst];
        for (const Obj& object : bases)
            changed |= addAll(heap_[{object.first, object.second, edge.field}], pts_[src]);
        break;
    }
    case Label::Load: // base --load[f]--> dst
    {
        const ObjSet bases = pts_[src];
        for (const Obj& object : bases)
            changed |= addAll(pts_[dst], readField(object, edge.field));
        break;
    }
    }
    return changed;
}

bool Solver::applyCall(const Edge& edge, CtxId callerCtx)
{
    const CtxId calleeCtx = push(edge.callSite, callerCtx, options_.k);
    bool changed = false;
    if (edge.label == Label::Dispatch) // r#c --dispatch[t] ĉ--> this: objects of type t only
    {
        const ObjSet receivers = pts_[{edge.src, nodeCtx(edge.src, callerCtx)}];
        for (const Obj& receiver : receivers)
        {
            if (graph_.nodes()[receiver.first].type != edge.type)
                continue;
            changed |= reach(graph_.nodes()[edge.dst].function, calleeCtx);
            changed |= pts_[{edge.dst, calleeCtx}].insert(receiver).second;
        }
    }
    else if (edge.dir == CallDir::Enter) // actual (caller) → formal (callee)
    {
        changed |= reach(graph_.nodes()[edge.dst].function, calleeCtx);
        changed |= addAll(pts_[{edge.dst, nodeCtx(edge.dst, calleeCtx)}],
                          pts_[{edge.src, nodeCtx(edge.src, callerCtx)}]);
    }
    else // formal return (callee) → actual return (caller)
    {
        changed |= addAll(pts_[{edge.dst, nodeCtx(edge.dst, callerCtx)}],
                          pts_[{edge.src, nodeCtx(edge.src, calleeCtx)}]);
    }
    return changed;
}

bool Solver::applyVirtualCall(CallSiteId siteId, CtxId callerCtx)
{
    const CallSite& site = graph_.callSites()[static_cast<std::size_t>(siteId)];
    if (site.actuals.empty() || !site.actuals[0])
        return false;
    const CtxId calleeCtx = push(siteId, callerCtx, options_.k);
    const ObjSet receivers = pts_[{*site.actuals[0], nodeCtx(*site.actuals[0], callerCtx)}];

    bool changed = false;
    for (const Obj& receiver : receivers)
    {
        // DynTypeOf(O): fixed at O's allocation (Builder::assignTypes).
        const TypeId type = graph_.nodes()[receiver.first].type;
        if (type == kUnknownType)
            continue;

        for (const VirtualTarget& target : site.targets)
        {
            const bool dispatches =
                std::find(target.types.begin(), target.types.end(), type) != target.types.end();
            if (!dispatches || target.formals.empty())
                continue;
            const std::string& callee = graph_.nodes()[target.formals[0]].function;
            changed |= reach(callee, calleeCtx);
            // [I-VCall]: the receiver object goes to `this` of its own target only ...
            changed |= pts_[{target.formals[0], calleeCtx}].insert(receiver).second;
            // ... the other actuals to every target it dispatches to.
            for (std::size_t i = 1; i < target.formals.size() && i < site.actuals.size(); ++i)
                if (site.actuals[i])
                    changed |= addAll(pts_[{target.formals[i], calleeCtx}],
                                      pts_[{*site.actuals[i], nodeCtx(*site.actuals[i], callerCtx)}]);
            if (target.ret && site.actualRet)
                changed |= addAll(pts_[{*site.actualRet, nodeCtx(*site.actualRet, callerCtx)}],
                                  pts_[{*target.ret, calleeCtx}]);
        }
    }
    return changed;
}

void Solver::solve()
{
    reach(kGlobalScope, 0);
    reach(kEntry, 0);
    if (options_.allFunctionsReachable)
        for (const Node& node : graph_.nodes())
            if (!node.function.empty())
                reach(node.function, 0);

    bool changed = true;
    while (changed)
    {
        changed = false;
        ++iterations_;
        const auto functions = methodCtx_; // snapshot: new contexts are picked up next round
        for (const auto& [function, contexts] : functions)
        {
            for (CtxId ctx : contexts)
            {
                if (auto it = intraByFunction_.find(function); it != intraByFunction_.end())
                    for (EdgeId e : it->second)
                        changed |= applyIntra(graph_.edges()[e], ctx);
                if (auto it = callsByCaller_.find(function); it != callsByCaller_.end())
                    for (EdgeId e : it->second)
                        changed |= applyCall(graph_.edges()[e], ctx);
                if (auto it = virtualSitesByCaller_.find(function); it != virtualSitesByCaller_.end())
                    for (CallSiteId c : it->second)
                        changed |= applyVirtualCall(c, ctx);
            }
        }
    }
}

std::set<NodeId> Solver::pts(NodeId n) const
{
    std::set<NodeId> result;
    for (auto it = pts_.lower_bound({n, 0}); it != pts_.end() && it->first.first == n; ++it)
        for (const Obj& object : it->second)
            result.insert(object.first);
    return result;
}

std::size_t Solver::methodContextCount() const
{
    std::size_t count = 0;
    for (const auto& entry : methodCtx_)
        count += entry.second.size();
    return count;
}

} // namespace ldc
