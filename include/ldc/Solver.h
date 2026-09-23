// Solver — L_F reachability on LDGraph, k = 0 (milestone M2).
//
// O ∈ PTS(v) iff some path O → v spells a word in L_F:
//   flowsto → new flows*        flows → assign | store[f] alias load[f]
//   alias(x, y) iff x and y are reached by a common object.
// With fields resolved per object (the field heap below), this is the standard
// fixpoint for the L_F relation: store[f] into b and load[f] out of b' meet
// exactly when b and b' share an object O, i.e. through the alias U-turn at O.
//
// Call edges (ĉ / č) are treated as plain assign here: no contexts yet (M3).
// A naive round-robin fixpoint for now; a worklist comes with contexts.

#pragma once

#include "ldc/LDGraph.h"

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace ldc
{

class Solver
{
public:
    explicit Solver(const LDGraph& graph);

    void solve();

    /// Objects (LDGraph node ids) that node `n` may point to.
    const std::set<NodeId>& pts(NodeId n) const { return pts_[n]; }

    std::size_t iterations() const { return iterations_; }

private:
    using FieldKey = std::pair<NodeId, FieldId>; // (object, field)

    bool addAll(std::set<NodeId>& into, const std::set<NodeId>& from);
    std::set<NodeId> readField(NodeId object, FieldId field) const;

    const LDGraph& graph_;
    std::vector<std::set<NodeId>> pts_;
    std::map<FieldKey, std::set<NodeId>> heap_;
    std::size_t iterations_ = 0;
};

} // namespace ldc
