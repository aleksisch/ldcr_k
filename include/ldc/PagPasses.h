#pragma once

#include "ldc/Cfl.h"
#include "ldc/SimplifiedPAG.h"

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace ldc {

// The PAG the L_DCR grammars run on, with only the edges of `functions`:
// - the callee's `this` gets a copy per dispatch site c, this#c, which only the dispatch at c
//   reaches and which flows into `this`; the callee side of the synthetic argument and result
//   fields (Dispatch.cpp) moves onto the copies, marked with their site, so that a value passed
//   at c is read back only by the instance dispatched at c (the call-site part of L_R);
// - every edge is also present walked backwards (Edge::reversed).
// The PAG's nodes keep their indices; the copies of `this` come after them.
frontend::SimplifiedPAG postProcessPAG(const frontend::SimplifiedPAG& pag,
                                       const std::set<std::string>& functions);

// Mutual refinement: solves the languages, keeps the pairs from an object to a variable that all
// of them reach (at the endpoints), and removes the edges that are not on a path of every language
// between such a pair, until no edge is removed. The languages take turns, each solved on the PAG
// the previous ones pruned. A path in all the languages at once keeps its edges, so no fact of
// their intersection is lost. Returns the number of edges removed.
std::size_t refinePAG(frontend::SimplifiedPAG& pag, const std::vector<cfl::Language>& languages,
                      const cfl::Options& options = {});

} // namespace ldc
