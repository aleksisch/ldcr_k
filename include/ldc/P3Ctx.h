// P3Ctx — selective context sensitivity that preserves kCFA's precision (He, Lu, Xue, ECOOP 2024,
// §4, Fig. 10–11).

#pragma once

#include "ldc/LDGraph.h"

#include <vector>

namespace ldc {

/// The nodes that need no context: kCFA with them analysed in context [] derives the same
/// points-to facts, contexts dropped (paper Theorem 7). Direct and function-pointer calls are
/// treated as calls on a dummy receiver, as the paper does for Java's static calls.
std::vector<bool> contextInsensitiveNodes(const LDGraph& graph);

} // namespace ldc
