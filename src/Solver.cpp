#include "ldc/Solver.h"

namespace ldc
{

Solver::Solver(const LDGraph& graph) : graph_(graph), pts_(graph.nodes().size())
{
}

bool Solver::addAll(std::set<NodeId>& into, const std::set<NodeId>& from)
{
    const std::size_t before = into.size();
    into.insert(from.begin(), from.end());
    return into.size() != before;
}

/// Field read with array-insensitive `*`: load[f] also sees store[*], and load[*]
/// sees every field of the object.
std::set<NodeId> Solver::readField(NodeId object, FieldId field) const
{
    std::set<NodeId> result;
    if (field == kAnyField)
    {
        for (auto it = heap_.lower_bound({object, kAnyField});
             it != heap_.end() && it->first.first == object; ++it)
            result.insert(it->second.begin(), it->second.end());
        return result;
    }
    for (FieldId f : {field, kAnyField})
    {
        auto it = heap_.find({object, f});
        if (it != heap_.end())
            result.insert(it->second.begin(), it->second.end());
    }
    return result;
}

void Solver::solve()
{
    for (const Edge& edge : graph_.edges())
        if (edge.label == Label::New)
            pts_[edge.dst].insert(edge.src);

    bool changed = true;
    while (changed)
    {
        changed = false;
        ++iterations_;
        for (const Edge& edge : graph_.edges())
        {
            switch (edge.label)
            {
            case Label::New:
                break;
            case Label::Assign:
                changed |= addAll(pts_[edge.dst], pts_[edge.src]);
                break;
            case Label::Store: // src --store[f]--> base: every object of base gets src in field f
                for (NodeId object : pts_[edge.dst])
                    changed |= addAll(heap_[{object, edge.field}], pts_[edge.src]);
                break;
            case Label::Load: // base --load[f]--> dst
                for (NodeId object : pts_[edge.src])
                    changed |= addAll(pts_[edge.dst], readField(object, edge.field));
                break;
            }
        }
    }
}

} // namespace ldc
