#include "ldc/Grammar.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace ldc::cfl;
using namespace ldc::frontend;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

// The PAG edge whose call-site terminal (Cfl.h) is `terminal` at `site`.
Edge edgeFor(const std::string& terminal, std::int32_t site, std::uint32_t from) {
    Edge e{NodeId{from}, NodeId{from + 1}, Label::Assign};
    if (terminal == "e") return e;
    e.callSite = CallSiteId{site};
    if (terminal == "call") e.dir = CallDir::Enter;
    if (terminal == "ret") e.dir = CallDir::Exit;
    if (terminal == "bc") e.dir = CallDir::DispatchExit;
    if (terminal == "bo") {
        e.label = Label::Store;
        e.field = FieldOffset{-11};
        e.dir = CallDir::DispatchEnter;
    }
    return e;
}

// Whether `grammar` derives S for the single path 0 -> 1 -> ... spelling `terminals`.
bool accepts(const Grammar& grammar, const std::vector<std::pair<std::string, int>>& terminals,
             const Options& options) {
    SimplifiedPAG path;
    for (std::size_t n = 0; n <= terminals.size(); ++n) path.addNode({});
    for (std::uint32_t i = 0; i < terminals.size(); ++i)
        path.addEdge(edgeFor(terminals[i].first, terminals[i].second, i));
    auto solved = solve(path, grammar, options);
    for (const auto& [u, v] : solved["S"])
        if (u == 0 && v == terminals.size()) return true;
    return false;
}

void check(const Options& options) {
    const auto lr = ldc::grammar::lr();
    // Paper Fig. 9: an excursion at c8 climbs out through c6 and must come back through c6.
    require(
        accepts(lr, {{"e", -1}, {"bo", 8}, {"ret", 6}, {"e", -1}, {"call", 6}, {"bc", 8}}, options),
        "L_R rejects a mirrored excursion");
    require(!accepts(lr, {{"bo", 8}, {"ret", 6}, {"e", -1}, {"call", 7}, {"bc", 8}}, options),
            "L_R accepts an excursion returning through another call site");
    // Fig. 8: opened at c4, closed at c5.
    require(!accepts(lr, {{"bo", 4}, {"e", -1}, {"bc", 5}}, options),
            "L_R accepts an excursion closed at another site");
    // An unclosed excursion.
    require(!accepts(lr, {{"bo", 8}, {"e", -1}}, options), "L_R accepts an unclosed excursion");
    // Outside excursions anything goes, and a call into another function and back inside one.
    require(accepts(lr, {{"ret", 6}, {"call", 9}, {"bo", 8}, {"call", 5}, {"ret", 5}, {"bc", 8}},
                    options),
            "L_R rejects calls outside or balanced calls inside an excursion");
}

int main() {
    check({Options::Solver::Svf});
    return 0;
}
