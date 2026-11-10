#include "ldc/Cfl.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace ldc::cfl;
using namespace ldc::frontend;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

SimplifiedPAG withNodes(std::size_t count) {
    SimplifiedPAG pag;
    for (std::size_t n = 0; n < count; ++n) pag.addNode({});
    return pag;
}

void check(const Options& options) {
    // LAGraph's example, with a = assign and b = new: S -> a S b | a b.
    //   0 -a-> 1 -a-> 2 -b-> 3 -b-> 4,  1 -a-> 5 -b-> 2
    auto pag = withNodes(6);
    for (auto [u, v] : {std::pair{0u, 1u}, {1u, 2u}, {1u, 5u}})
        pag.addEdge({NodeId{u}, NodeId{v}, Label::Assign});
    for (auto [u, v] : {std::pair{2u, 3u}, {3u, 4u}, {5u, 2u}})
        pag.addEdge({NodeId{u}, NodeId{v}, Label::New});
    Grammar grammar{
        {"S", "A", "M"}, {"S", "A", "B"}, {"M", "S", "B"}, {"A", "assign"}, {"B", "new"}};
    auto pairs = solve(pag, grammar, options)["S"];
    std::sort(pairs.begin(), pairs.end());
    require(pairs == Pairs{{0, 3}, {0, 4}, {1, 2}, {1, 3}}, "Unexpected S pairs");

    // A family: a store and a load match only on the same field.
    //   0 -store[0]-> 1 -load[0]-> 2,  0 -store[1]-> 3 -load[0]-> 4
    auto fields = withNodes(5);
    fields.addEdge({NodeId{0}, NodeId{1}, Label::Store, FieldOffset{0}});
    fields.addEdge({NodeId{1}, NodeId{2}, Label::Load, FieldOffset{0}});
    fields.addEdge({NodeId{0}, NodeId{3}, Label::Store, FieldOffset{1}});
    fields.addEdge({NodeId{3}, NodeId{4}, Label::Load, FieldOffset{0}});
    Grammar dyck{{"S", "O_i", "C_i"}, {"O_i", "store_i"}, {"C_i", "load_i"}};
    auto family = solve(fields, dyck, options);
    require(family["S"] == Pairs{{0, 2}}, "Unexpected family pairs");
    require(family["O_i[1]"] == Pairs{{0, 3}}, "Unexpected family member pairs");

    // Two languages over one PAG: a^n b^n and paths of two edges (E -> A, B: rules between
    // nonterminals).
    Grammar two{{"L", "E", "E"}, {"E", "A"}, {"E", "B"}, {"A", "assign"}, {"B", "new"}};
    require(solve(pag, {{grammar, "S"}, {two, "L"}}, options) == Pairs{{1, 2}, {1, 3}},
            "Unexpected intersection");
}

void checkPaths() {
    // LAGraph's example: S -> a S b | a b, with edges 0..5 in the order added.
    //   0 -a-> 1 -a-> 2 -b-> 3 -b-> 4,  1 -a-> 5 -b-> 2
    auto pag = withNodes(6);
    for (auto [u, v] : {std::pair{0u, 1u}, {1u, 2u}, {1u, 5u}})
        pag.addEdge({NodeId{u}, NodeId{v}, Label::Assign});
    for (auto [u, v] : {std::pair{2u, 3u}, {3u, 4u}, {5u, 2u}})
        pag.addEdge({NodeId{u}, NodeId{v}, Label::New});
    Grammar grammar{
        {"S", "A", "M"}, {"S", "A", "B"}, {"M", "S", "B"}, {"A", "assign"}, {"B", "new"}};
    const auto found = paths(pag, grammar, "S", {{0, 4}, {0, 3}, {0, 5}});
    require(found[0] == std::vector<std::size_t>{0, 1, 3, 4}, "Unexpected path 0 ~> 4");
    require(found[1] == std::vector<std::size_t>{0, 2, 5, 3}, "Unexpected path 0 ~> 3");
    require(!found[2], "Unexpected path 0 ~> 5");

    // A family: the path takes the store and the load of the same field.
    auto fields = withNodes(5);
    fields.addEdge({NodeId{0}, NodeId{1}, Label::Store, FieldOffset{0}});
    fields.addEdge({NodeId{1}, NodeId{2}, Label::Load, FieldOffset{0}});
    fields.addEdge({NodeId{0}, NodeId{3}, Label::Store, FieldOffset{1}});
    fields.addEdge({NodeId{3}, NodeId{4}, Label::Load, FieldOffset{0}});
    Grammar dyck{{"S", "O_i", "C_i"}, {"O_i", "store_i"}, {"C_i", "load_i"}};
    const auto family = paths(fields, dyck, "S", {{0, 2}, {0, 4}});
    require(family[0] == std::vector<std::size_t>{0, 1}, "Unexpected family path");
    require(!family[1], "Unexpected path through different fields");
}

int main() {
    check({Options::Solver::LAGraph});
    check({Options::Solver::Svf});
    checkPaths();
    return 0;
}
