#pragma once

#include "ldc/SimplifiedPAG.h"

#include <cstdint>
#include <map>
#include <ostream>
#include <set>
#include <string>

namespace ldc {

// Variable -> objects it may point to, as SimplifiedPAG::nodes() indices.
using PointsTo = std::map<std::uint32_t, std::set<std::uint32_t>>;

// One `var -> obj` line per fact, sorted, stable between runs: variables by
// `<function>::<source name>`, objects by the `// <label>` comment on their line in `source`.
void writeFacts(std::ostream& out, const frontend::SimplifiedPAG& graph, const PointsTo& facts,
                const std::string& source);

} // namespace ldc
