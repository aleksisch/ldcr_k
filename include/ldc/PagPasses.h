#pragma once

#include "ldc/SimplifiedPAG.h"

#include <set>
#include <string>

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

} // namespace ldc
