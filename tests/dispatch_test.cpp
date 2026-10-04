#include "ldc/SVFFrontend.h"
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <tuple>

using namespace ldc::frontend;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    require(argc == 2, "Expected the dispatch fixture's LLVM IR path");
    const auto graph = analyzeModules({argv[1]}).graph;
    const auto& sites = graph.callSites();

    // Looks up graph entities by the fixture's names; the checks below compare IDs only.
    const auto site = [&](const std::string& caller) {
        for (auto i = 0u; i < sites.size(); ++i) {
            if (sites[i].flags.isVirtual && sites[i].caller == caller) {
                return CallSiteId{static_cast<std::int32_t>(i)};
            }
        }
        require(false, "No virtual call in " + caller);
        return CallSiteId{};
    };
    const auto vtable = [&](const std::string& cls) {
        for (auto i = 0u; i < graph.nodes().size(); ++i) {
            if (graph.nodes()[i].kind == NodeKind::Obj && graph.nodes()[i].name == cls) {
                return NodeId{i};
            }
        }
        require(false, "No vtable of " + cls);
        return NodeId{};
    };
    const auto self = [&](const std::string& method) {
        for (const auto& info : sites) {
            for (const auto& target : info.targets) {
                if (target.function == method && !target.formals.empty() && target.formals[0]) {
                    return *target.formals[0];
                }
            }
        }
        require(false, "No call target " + method);
        return NodeId{};
    };

    const auto local = [&](const std::string& name) {
        for (auto i = 0u; i < graph.nodes().size(); ++i) {
            const auto& node = graph.nodes()[i];
            if (node.kind == NodeKind::Var && node.function == "main" && node.sourceName == name) {
                return NodeId{i};
            }
        }
        require(false, "No local variable " + name);
        return NodeId{};
    };

    // (call site, class vtable, callee this) of every dispatch edge.
    using Dispatch = std::tuple<std::int32_t, std::uint32_t, std::uint32_t>;
    const auto dispatch = [](CallSiteId c, NodeId cls, NodeId callee) {
        return Dispatch{c.value, cls.value, callee.value};
    };
    std::set<Dispatch> dispatches;
    // (allocated variable, class vtable) of every typed allocation edge.
    std::set<std::pair<std::uint32_t, std::uint32_t>> news;
    auto calleeLoads = 0;
    for (const auto& edge : graph.edges()) {
        if (edge.label == Label::Dispatch) {
            dispatches.insert(dispatch(*edge.callSite, *edge.type, edge.dst));
        }
        if (edge.label == Label::New && edge.type) news.insert({edge.dst.value, edge.type->value});
        if (edge.label == Label::Load && edge.src == self("Square::area(int*)")) ++calleeLoads;
        require(!(edge.callSite == site("callReset(Shape*)") &&
                  (edge.dir == CallDir::Exit || edge.dir == CallDir::DispatchExit)),
                "Void virtual call must not have a return edge");
    }

    const auto area = site("callArea(Shape*, int*)");
    const auto areaAgain = site("callAreaAgain(Shape*, int*)");
    const auto reset = site("callReset(Shape*)");
    const auto square = vtable("Square");
    const auto circle = vtable("Circle");
    // Abstract Shape is never a receiver; Square inherits Shape::reset; Unrelated is excluded.
    require(dispatches == std::set<Dispatch>{
                dispatch(area, square, self("Square::area(int*)")),
                dispatch(area, circle, self("Circle::area(int*)")),
                dispatch(areaAgain, square, self("Square::area(int*)")),
                dispatch(areaAgain, circle, self("Circle::area(int*)")),
                dispatch(reset, square, self("Shape::reset()")),
                dispatch(reset, circle, self("Circle::reset()")),
            },
            "Unexpected dispatch edges");
    // Stack and heap objects take the class of their constructor; Shape (a base) never does.
    require(news == std::set<std::pair<std::uint32_t, std::uint32_t>>{
                {local("square").value, square.value},
                {local("circle").value, circle.value},
                {local("heap").value, circle.value},
                {local("unrelated").value, vtable("Unrelated").value},
            },
            "Unexpected typed allocations");
    // The callee side is shared by both call sites.
    require(calleeLoads == 1, "Callee parameter load must be added once per method");
}
