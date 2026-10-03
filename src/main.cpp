// ldc — L_DC prototype driver.

#include "ldc/Builder.h"
#include "ldc/SVFFrontend.h"
#include "SVFDetails.h"
#include "ldc/LDGraph.h"
#include "ldc/P3Ctx.h"
#include "ldc/Solver.h"

#include "Graphs/CallGraph.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace SVF;

namespace {
/// Σ|pts| over variables (contexts merged), virtual call edges, polymorphic call sites.
void printResult(const ldc::LDGraph& graph, const ldc::Solver& solver) {
    std::size_t total = 0;
    for (ldc::NodeId n = 0; n < graph.nodes().size(); ++n)
        if (graph.nodes()[n].kind == ldc::NodeKind::Var) total += solver.pts(n).size();
    std::map<ldc::CallSiteId, std::size_t> targets;
    for (const auto& edge : solver.virtualCallEdges()) ++targets[edge.first];
    std::size_t polymorphic = 0;
    for (const auto& entry : targets) polymorphic += entry.second > 1;
    std::cout << "Result: sum |pts| " << total << ", virtual call edges "
              << solver.virtualCallEdges().size() << " (" << polymorphic
              << " polymorphic sites), untyped receivers " << solver.untypedReceivers().size()
              << "\n";
}

/// "E::foo(G*)" -> "E::foo".
std::string functionKey(const std::string& name) { return name.substr(0, name.find('(')); }

/// Source names of variables: after mem2reg, `x` in `O* x = id(a)` is %call, and `x` survives
/// only in debug records. Formals keep their IR names.
std::unordered_map<ldc::SvfId, std::string> debugNames() {
    std::unordered_map<ldc::SvfId, std::string> names;
    for (const auto& [id, source] : ldc::frontend::detail::debugNames())
        names.emplace(id.value, functionKey(source.function) + "::" + source.name);
    return names;
}

/// Source line -> label, for lines with a `// <label>` comment.
std::unordered_map<int, std::string> lineLabels(const std::string& path) {
    std::unordered_map<int, std::string> labels;
    std::ifstream in(path);
    std::string line, label;
    for (int number = 1; std::getline(in, line); ++number)
        if (auto c = line.find("//"); c != std::string::npos)
            if (std::istringstream(line.substr(c + 2)) >> label) labels.emplace(number, label);
    return labels;
}

/// Text of every node, stable between runs: variables by their debug name, objects by the
/// label of their allocation line.
std::vector<std::string> nodeTexts(const ldc::LDGraph& graph, const std::string& sourceFile) {
    const auto names = debugNames();
    const auto labels = lineLabels(sourceFile);
    std::vector<std::string> texts;
    for (const ldc::Node& node : graph.nodes()) {
        if (node.kind == ldc::NodeKind::Obj) {
            auto it = labels.find(node.line);
            texts.push_back(it != labels.end() ? it->second : node.name);
            continue;
        }
        auto it = names.find(node.svfId);
        texts.push_back(it != names.end() ? it->second
                                          : functionKey(node.function) + "::" + node.name + "@" +
                                                std::to_string(node.line));
    }
    return texts;
}

/// A call site by caller and line (ids differ between runs).
std::string siteText(const ldc::CallSite& site) {
    return functionKey(site.caller) + ":" + std::to_string(site.line);
}

/// One `var[ ctx ] -> obj[ heap ctx ]` line per fact.
void writeFacts(const ldc::LDGraph& graph, const ldc::Solver& solver, const std::string& path,
                const std::string& sourceFile) {
    const auto texts = nodeTexts(graph, sourceFile);
    auto context = [&](const ldc::Solver::CallString& ctx) {
        std::string t = "[";
        for (ldc::CallSiteId c : ctx)
            t += " " + siteText(graph.callSites()[static_cast<std::size_t>(c)]);
        return t + " ]";
    };
    std::set<std::string> lines;
    for (const auto& [var, objects] : solver.contextFacts())
        for (const auto& [object, heap] : objects)
            lines.insert(texts[var.first] + context(var.second) + " -> " + texts[object] +
                         context(heap));
    std::ofstream out(path);
    for (const std::string& line : lines) out << line << "\n";
}

/// The graph as tables for the CFL-reachability pipeline (cfl/ldcr.py), with P3Ctx's choice:
/// nodes.tsv (id kind function type fieldLimit insensitive text), edges.tsv (src dst label
/// field type site dir), sites.tsv (id caller text), subtypes.tsv (object offset type).
void writeExport(const ldc::LDGraph& graph, const std::vector<bool>& insensitive,
                 const std::string& dir, const std::string& sourceFile) {
    std::filesystem::create_directories(dir);
    const auto texts = nodeTexts(graph, sourceFile);
    std::ofstream nodes(dir + "/nodes.tsv");
    for (ldc::NodeId n = 0; n < graph.nodes().size(); ++n) {
        const ldc::Node& node = graph.nodes()[n];
        nodes << n << "\t" << static_cast<int>(node.kind) << "\t" << node.function << "\t"
              << node.type << "\t" << node.fieldLimit << "\t"
              << (n < insensitive.size() && insensitive[n]) << "\t" << texts[n] << "\n";
    }
    std::ofstream edges(dir + "/edges.tsv");
    for (const ldc::Edge& e : graph.edges())
        edges << e.src << "\t" << e.dst << "\t" << static_cast<int>(e.label) << "\t" << e.field
              << "\t" << e.type << "\t" << e.callSite << "\t" << static_cast<int>(e.dir) << "\n";
    std::ofstream sites(dir + "/sites.tsv");
    for (std::size_t c = 0; c < graph.callSites().size(); ++c)
        sites << c << "\t" << graph.callSites()[c].caller << "\t" << siteText(graph.callSites()[c])
              << "\n";
    std::ofstream subtypes(dir + "/subtypes.tsv");
    for (const auto& [key, type] : graph.subobjectTypes())
        subtypes << key.first << "\t" << key.second << "\t" << type << "\n";
}

/// Variables whose objects differ from SVF Andersen's (base objects present in LDGraph).
std::size_t compareWithAndersen(const ldc::LDGraph& graph, const ldc::Solver& solver,
                                PointerAnalysis& andersen) {
    std::size_t mismatches = 0;
    for (ldc::NodeId n = 0; n < graph.nodes().size(); ++n) {
        const ldc::Node& node = graph.nodes()[n];
        if (node.kind != ldc::NodeKind::Var) continue;
        std::set<ldc::SvfId> ours, theirs;
        for (ldc::NodeId o : solver.pts(n))
            if (graph.nodes()[o].svfId != ldc::kNoSvfId) // construction sites: ours only
                ours.insert(graph.nodes()[o].svfId);
        for (NodeID o : andersen.getPts(node.svfId))
            if (auto obj = graph.findSvf(andersen.getBaseObjVarID(o));
                obj && graph.nodes()[*obj].kind == ldc::NodeKind::Obj)
                theirs.insert(andersen.getBaseObjVarID(o));
        if (ours == theirs) continue;
        ++mismatches;
        std::cout << "  MISMATCH " << functionKey(node.function) << "::" << node.name << "\n";
    }
    return mismatches;
}

} // namespace

int main(int argc, char** argv) {
    const auto cli = ldc::parseCommandLine(argc, argv);
    if (!cli.error.empty()) {
        std::cerr << "ldc: " << cli.error << "\n";
        return 1;
    }
    if (cli.help) {
        ldc::printHelp(std::cout);
        return 0;
    }
    const auto& modules = cli.modules;

    if (!cli.programDot.empty()) {
        const auto result = ldc::frontend::analyzeModules(modules);
        result.printSummary(std::cout);
        std::ofstream out(cli.programDot);
        result.graph.dumpDot(out);
        out.close();
        if (!out) {
            std::cerr << "Cannot write program graph: " << cli.programDot << "\n";
            return 1;
        }
        return 0;
    }

    // Build the module in memory without writing preprocessing bitcode files.
    LLVMModuleSet::buildSVFModule(modules);

    SVFIRBuilder builder;
    SVFIR* pag = builder.build();
    auto ander = ldc::frontend::detail::runAndersen(*pag);

    // Andersen augments the initial graph with resolved indirect and virtual calls.
    // Print stable, unique caller/callee pairs; SVF retains the call-site information.
    std::set<std::pair<std::string, std::string>> edges;
    for (const auto& entry : *ander->getCallGraph()) {
        const CallGraphNode* caller = entry.second;
        for (const CallGraphEdge* edge : caller->getOutEdges())
            edges.emplace(caller->getName(), edge->getDstNode()->getName());
    }
    for (const auto& edge : edges)
        std::cout << "call: " << edge.first << " -> " << edge.second << "\n";

    ldc::SolverOptions options;
    options.k = cli.k;
    if (cli.mode == "lfc")
        options.mode = ldc::Mode::Lfc;
    else if (cli.mode == "kcfa")
        options.mode = ldc::Mode::Kcfa;
    else if (cli.mode == "ldc")
        options.mode = ldc::Mode::Ldc;
    else if (cli.mode == "ldcr")
        options.mode = ldc::Mode::Ldcr;
    else {
        std::cerr << "ldc: unknown -ldc-mode=" << cli.mode << " (lfc | kcfa | ldc | ldcr)\n";
        return 2;
    }
    options.allFunctionsReachable = cli.compareAndersen;

    ldc::BuildStats stats;
    ldc::LDGraph graph = ldc::buildLDGraph(*pag, *ander->getCallGraph(), options.mode, stats);
    graph.printSummary(std::cout);
    stats.print(std::cout);
    if (cli.p3ctx) {
        options.insensitive = ldc::contextInsensitiveNodes(graph);
        std::size_t kept = 0;
        for (bool insensitive : options.insensitive) kept += !insensitive;
        std::cout << "P3Ctx: " << kept << " of " << graph.nodes().size()
                  << " nodes context-sensitive\n";
    }
    if (!cli.exportDir.empty()) {
        writeExport(graph, options.insensitive, cli.exportDir, cli.sourceFile);
        return 0;
    }

    ldc::Solver solver(graph, options);
    const auto start = std::chrono::steady_clock::now();
    solver.solve();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "Solver [" << cli.mode << ", k = " << options.k << "]: fixpoint after "
              << solver.iterations() << " steps, " << seconds << " s, " << solver.contextCount()
              << " contexts, " << solver.methodContextCount() << " (function, context) pairs\n";
    printResult(graph, solver);
    if (!cli.facts.empty()) writeFacts(graph, solver, cli.facts, cli.sourceFile);

    bool ok = true;
    if (cli.compareAndersen) {
        std::size_t mismatches = compareWithAndersen(graph, solver, *ander);
        std::cout << "Andersen comparison: " << mismatches << " mismatching variables\n";
        ok &= mismatches == 0;
    }
    if (!cli.analysisDot.empty()) {
        std::ofstream dot(cli.analysisDot);
        graph.dumpDot(dot);
        std::cout << "LDGraph written to " << cli.analysisDot << "\n";
    }
    ander.reset();
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
    return ok ? 0 : 1;
}
