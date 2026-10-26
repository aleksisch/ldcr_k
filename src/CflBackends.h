#pragma once

#include "ldc/Cfl.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ldc::cfl {

// The PAG as the backends take it: one edge per terminal, `label` without "_i" for a family.
struct Edge {
    std::uint32_t src;
    std::uint32_t dst;
    std::string label;
    std::int32_t index = -1; // index within the family, or -1
    std::size_t pagEdge = 0; // the PAG edge it reads
};

struct Graph {
    std::size_t nodes = 0;
    std::vector<Edge> edges;
};

// cfl::solve's backends, for a grammar without rules between nonterminals.
std::map<std::string, Pairs> solveWithSvf(const Graph& graph, const Grammar& grammar);
std::map<std::string, Pairs> solveWithLAGraph(const Graph& graph, const Grammar& grammar);

// cfl::paths over the graph: the edges (indices into graph.edges) of each path.
std::vector<std::optional<std::vector<std::size_t>>>
findPaths(const Graph& graph, const Grammar& grammar, const std::string& start, const Pairs& queries);

} // namespace ldc::cfl
