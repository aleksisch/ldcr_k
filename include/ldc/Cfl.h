#pragma once

#include "ldc/SimplifiedPAG.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Context-free-language reachability over a PAG. Each edge of the PAG is a terminal of two
// kinds, named by its fields, "_i" marking a family indexed as given (a rule mentioning families
// applies once per index, the same index for all of them):
// - by its label: new<T> (new for an untyped allocation, T the class's vtable node), assign,
//   gep_i (i the field + 2, 0 for an unknown field), store_i / load_i (i the field), storeAny /
//   loadAny (an unknown field), storeArg_i / loadArg_i (a synthetic argument or result field,
//   i one per (field, call site)), dispatch<T>;
//   for the C++ heap model (addObjectFields): shift<T>_i (i the gep field + 2, T the class of the
//   object it leads to, none for an object at an offset), cell_i, starCell_i, starCellAny, hub_i,
//   hubAny (i the field) and cellHub;
//   a reversed edge (Edge::reversed) gets "_r" after the name;
// - by its call site c: call_i for a call or a dispatch at c, ret_i for a return, bo_i and bc_i for
//   the start and the end of a dispatch excursion (DispatchEnter, DispatchExit), e otherwise; a
//   reversed edge swaps call and ret, bo and bc.
namespace ldc::cfl {

// lhs -> a b, or lhs -> a when b is empty; `a` may be a nonterminal (lhs -> A).
struct Rule {
    std::string lhs, a, b;
};

using Grammar = std::vector<Rule>;

using Pairs = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

struct Options {
    // The first solver is the default.
    enum class Solver {
        // LAGraph_CFL_reachability_adv
        LAGraph,
        // SVF's CFLSolver
        Svf,
    };
    Solver solver{};
};

// The pairs (u, v) of PAG nodes with a path u ~> v derived from each nonterminal; index k of a
// family "N_i" is "N_i[k]".
std::map<std::string, Pairs> solve(const frontend::SimplifiedPAG& pag, const Grammar& grammar,
                                   const Options& options = {});

// A language over the PAG: its grammar and start symbol.
struct Language {
    Grammar grammar;
    std::string start;
};

// The pairs (u, v) reachable in every language, sorted.
Pairs solve(const frontend::SimplifiedPAG& pag, const std::vector<Language>& languages,
            const Options& options = {});

// For each query (u, v), a path u ~> v derived from `start`, as the PAG edges it walks in order
// (reversed ones included), or nullopt. The path of a fact's first derivation in a worklist
// solver of our own: short, not necessarily the shortest.
std::vector<std::optional<std::vector<std::size_t>>> paths(const frontend::SimplifiedPAG& pag,
                                                           const Grammar& grammar,
                                                           const std::string& start,
                                                           const Pairs& queries);

// For each PAG edge, whether it is on some path u ~> v derived from `start` for a pair (u, v) of
// `pairs` (the pairs without such a path are skipped); `solved` is solve(pag, grammar).
std::vector<bool> usedEdges(const frontend::SimplifiedPAG& pag, const Grammar& grammar,
                            const std::map<std::string, Pairs>& solved, const std::string& start,
                            const Pairs& pairs);

} // namespace ldc::cfl
