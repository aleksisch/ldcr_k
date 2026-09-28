#include "ldc/Eval.h"

#include "ldc/Solver.h"

#include "MemoryModel/PointerAnalysis.h"

#include <chrono>
#include <iomanip>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ldc
{

namespace
{

using PtsMap = std::vector<std::set<NodeId>>; // by LDGraph node; empty for non-variables

struct Row
{
    std::string name;
    PtsMap pts;
    std::size_t reachedFunctions = 0;
    std::size_t methodContexts = 0;
    std::set<std::pair<CallSiteId, std::string>> callEdges;
    std::size_t callEdgeContexts = 0; ///< (site, caller context, callee); 0 = not applicable
    std::set<NodeId> untyped;
    double millis = 0;
    bool hasContexts = true;
};

bool isVariable(const Node& node)
{
    return node.kind == NodeKind::Var;
}

Row runSolver(const LDGraph& graph, Mode mode, unsigned k, const char* name)
{
    Row row;
    row.name = name;
    Solver solver(graph, SolverOptions{k, mode, false});
    const auto start = std::chrono::steady_clock::now();
    solver.solve();
    row.millis =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    row.pts.resize(graph.nodes().size());
    for (NodeId n = 0; n < graph.nodes().size(); ++n)
        if (isVariable(graph.nodes()[n]))
            row.pts[n] = solver.pts(n);
    row.reachedFunctions = solver.reachedFunctionCount();
    row.methodContexts = solver.methodContextCount();
    row.callEdges = solver.virtualCallEdges();
    row.callEdgeContexts = solver.virtualCallEdgeContextCount();
    row.untyped = solver.untypedReceivers();
    return row;
}

/// SVF Andersen: every function analysed, no contexts. Its virtual call edges are the `Fc`
/// edges Builder wired from its call graph.
Row andersenRow(const LDGraph& graph, SVF::PointerAnalysis& andersen)
{
    Row row;
    row.name = "SVF Andersen";
    row.hasContexts = false;
    row.pts.resize(graph.nodes().size());
    for (NodeId n = 0; n < graph.nodes().size(); ++n)
    {
        const Node& node = graph.nodes()[n];
        if (!isVariable(node))
            continue;
        for (SVF::NodeID o : andersen.getPts(node.svfId))
            if (auto obj = graph.findSvf(andersen.getBaseObjVarID(o));
                obj && graph.nodes()[*obj].kind == NodeKind::Obj)
                row.pts[n].insert(*obj);
    }
    for (const Edge& edge : graph.edges())
        if (edge.encoding == Encoding::Fc && edge.dir == CallDir::Enter)
            row.callEdges.insert({edge.callSite, graph.nodes()[edge.dst].function});
    return row;
}

std::size_t polymorphicSites(const Row& row)
{
    std::map<CallSiteId, std::size_t> targets;
    for (const auto& edge : row.callEdges)
        ++targets[edge.first];
    std::size_t count = 0;
    for (const auto& entry : targets)
        count += entry.second > 1;
    return count;
}

} // namespace

std::size_t evaluate(const LDGraph& graph, SVF::PointerAnalysis& andersen, unsigned k,
                     std::ostream& os)
{
    std::vector<Row> rows;
    rows.push_back(runSolver(graph, Mode::Kcfa, k, "kCFA"));
    rows.push_back(runSolver(graph, Mode::Ldc, k, "L_DC"));
    rows.push_back(runSolver(graph, Mode::Lfc, k, "L_FC"));
    rows.push_back(andersenRow(graph, andersen));
    const Row& reference = rows.front();

    std::size_t virtualSites = 0;
    for (const CallSite& site : graph.callSites())
        virtualSites += site.isVirtual;

    os << "k = " << k << ", " << virtualSites << " virtual call sites\n\n"
       << "| analysis | Σ\\|pts\\| | vars ≠ kCFA | extra | missing | functions | (fn, ctx) | "
          "vcall edges | poly sites | vcall edges × ctx | untyped recv | ms |\n"
       << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";

    std::size_t missingTotal = 0;
    for (const Row& row : rows)
    {
        std::size_t total = 0, differing = 0, extra = 0, missing = 0;
        for (NodeId n = 0; n < graph.nodes().size(); ++n)
        {
            total += row.pts[n].size();
            std::size_t add = 0, lack = 0;
            for (NodeId o : row.pts[n])
                add += reference.pts[n].count(o) == 0;
            for (NodeId o : reference.pts[n])
                lack += row.pts[n].count(o) == 0;
            differing += add + lack > 0;
            extra += add;
            missing += lack;
        }
        // Andersen analyses every function, not only those reachable from main: its extra
        // objects include unreachable code, and it is not counted as a soundness check.
        if (row.hasContexts)
            missingTotal += missing;

        std::ostringstream ms;
        ms << std::fixed << std::setprecision(1) << row.millis;
        auto opt = [&](std::size_t v) { return row.hasContexts ? std::to_string(v) : std::string("–"); };
        os << "| " << row.name << " | " << total << " | " << differing << " | " << extra << " | "
           << missing << " | " << opt(row.reachedFunctions) << " | " << opt(row.methodContexts)
           << " | " << row.callEdges.size() << " | " << polymorphicSites(row) << " | "
           << opt(row.callEdgeContexts) << " | " << opt(row.untyped.size()) << " | "
           << (row.hasContexts ? ms.str() : std::string("–")) << " |\n";
    }
    os << "\n";
    return missingTotal;
}

} // namespace ldc
