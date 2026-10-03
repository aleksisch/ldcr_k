// ldc — L_DC prototype driver.

#include "ldc/Builder.h"
#include "ldc/LDGraph.h"
#include "ldc/P3Ctx.h"
#include "ldc/Solver.h"

#include "Graphs/CallGraph.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugProgramInstruction.h>

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

const Option<std::string> DotOut("ldc-dot", "Write LDGraph to this Graphviz file", "");
const Option<u32_t> ContextDepth("ldc-k", "Context depth k", 0);
const Option<std::string> Mode("ldc-mode", "Analysis: lfc | kcfa | ldc | ldcr", "lfc");
const Option<std::string> SourceFile("ldc-src", "C++ source file, for object labels", "");
const Option<std::string> FactsOut("ldc-facts", "Write the facts to this file", "");
const Option<bool> P3Ctx("ldc-p3ctx", "Contexts only where they can matter (P3Ctx)", false);
const Option<std::string>
    ExportDir("ldc-export", "Write the graph for cfl/ldcr.py to this directory and stop", "");
const Option<bool> CompareAndersen("ldc-andersen", "Compare PTS with SVF Andersen", false);

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
    LLVMModuleSet* modules = LLVMModuleSet::getLLVMModuleSet();
    auto record = [&](const llvm::Value* value, const std::string& name) {
        if (value != nullptr && modules->hasValueNode(value))
            names.emplace(modules->getValueNode(value), name);
    };
    for (u32_t i = 0; i < modules->getModuleNum(); ++i)
        for (const llvm::Function& fn : *modules->getModule(i)) {
            const std::string function = functionKey(llvm::demangle(fn.getName().str()));
            for (const llvm::Argument& arg : fn.args())
                if (arg.hasName()) record(&arg, function + "::" + arg.getName().str());
            for (const llvm::BasicBlock& block : fn)
                for (const llvm::Instruction& inst : block)
                    for (const llvm::DbgVariableRecord& dvr :
                         llvm::filterDbgVars(inst.getDbgRecordRange()))
                        if (dvr.getVariable() != nullptr)
                            record(dvr.getVariableLocationOp(0),
                                   function + "::" + dvr.getVariable()->getName().str());
        }
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
std::vector<std::string> nodeTexts(const ldc::LDGraph& graph) {
    const auto names = debugNames();
    const auto labels = lineLabels(SourceFile());
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
void writeFacts(const ldc::LDGraph& graph, const ldc::Solver& solver, const std::string& path) {
    const auto texts = nodeTexts(graph);
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
                 const std::string& dir) {
    std::filesystem::create_directories(dir);
    const auto texts = nodeTexts(graph);
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
    std::vector<std::string> modules =
        OptionBase::parseOptions(argc, argv, "ldc: L_DC prototype", "[options] <input.ll>");
    if (modules.empty()) {
        std::cerr << "usage: ldc [options] <input.ll>\n";
        return 1;
    }

    // Same entry sequence as SVF's `wpa`, without LLVMModuleSet::preProcessBCs(): it writes
    // `<name>.pre.bc`, which fails in the svftools/svf image and crashes the process.
    LLVMModuleSet::buildSVFModule(modules);

    SVFIRBuilder builder;
    SVFIR* pag = builder.build();
    Andersen* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);

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
    options.k = ContextDepth();
    if (Mode() == "lfc")
        options.mode = ldc::Mode::Lfc;
    else if (Mode() == "kcfa")
        options.mode = ldc::Mode::Kcfa;
    else if (Mode() == "ldc")
        options.mode = ldc::Mode::Ldc;
    else if (Mode() == "ldcr")
        options.mode = ldc::Mode::Ldcr;
    else {
        std::cerr << "ldc: unknown -ldc-mode=" << Mode() << " (lfc | kcfa | ldc | ldcr)\n";
        return 2;
    }
    options.allFunctionsReachable = CompareAndersen();

    ldc::BuildStats stats;
    ldc::LDGraph graph = ldc::buildLDGraph(*pag, *ander->getCallGraph(), options.mode, stats);
    graph.printSummary(std::cout);
    stats.print(std::cout);
    if (P3Ctx()) {
        options.insensitive = ldc::contextInsensitiveNodes(graph);
        std::size_t kept = 0;
        for (bool insensitive : options.insensitive) kept += !insensitive;
        std::cout << "P3Ctx: " << kept << " of " << graph.nodes().size()
                  << " nodes context-sensitive\n";
    }
    if (!ExportDir().empty()) {
        writeExport(graph, options.insensitive, ExportDir());
        return 0;
    }

    ldc::Solver solver(graph, options);
    const auto start = std::chrono::steady_clock::now();
    solver.solve();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "Solver [" << Mode() << ", k = " << options.k << "]: fixpoint after "
              << solver.iterations() << " steps, " << seconds << " s, " << solver.contextCount()
              << " contexts, " << solver.methodContextCount() << " (function, context) pairs\n";
    printResult(graph, solver);
    if (!FactsOut().empty()) writeFacts(graph, solver, FactsOut());

    bool ok = true;
    if (CompareAndersen()) {
        std::size_t mismatches = compareWithAndersen(graph, solver, *ander);
        std::cout << "Andersen comparison: " << mismatches << " mismatching variables\n";
        ok &= mismatches == 0;
    }
    if (!DotOut().empty()) {
        std::ofstream dot(DotOut());
        graph.dumpDot(dot);
        std::cout << "LDGraph written to " << DotOut() << "\n";
    }
    AndersenWaveDiff::releaseAndersenWaveDiff();
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
    return ok ? 0 : 1;
}
