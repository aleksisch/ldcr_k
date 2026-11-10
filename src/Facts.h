#pragma once

#include "ldc/SimplifiedPAG.h"

#include <cstdint>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace ldc {

// Variable -> objects it may point to, as SimplifiedPAG::nodes() indices.
using PointsTo = std::map<std::uint32_t, std::set<std::uint32_t>>;

// One `var -> obj` line per fact, sorted, stable between runs: variables by
// `<function>::<source name>`, objects by the `// <label>` comment on their line in `source`.
void writeFacts(std::ostream& out, const frontend::SimplifiedPAG& graph, const PointsTo& facts,
                const std::string& source);

// The names writeFacts gives the nodes, by node index.
std::vector<std::string> nodeNames(const frontend::SimplifiedPAG& graph, const std::string& source);

// As nodeNames, with the objects without a `// <label>` made unique by their function and line:
// `<function>::<name>@<line>` (so two `call7` in different functions stay two objects).
std::vector<std::string> qualifiedNames(const frontend::SimplifiedPAG& graph,
                                        const std::string& source);

// As writeFacts, with the nodes named by `names`.
void writeFacts(std::ostream& out, const std::vector<std::string>& names, const PointsTo& facts);

} // namespace ldc
