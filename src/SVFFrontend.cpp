#include "ldc/SVFFrontend.h"
#include "SVFDetails.h"
#include "Graphs/CallGraph.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "WPA/Andersen.h"
#include <ostream>
#include <stdexcept>
#include "SVFIR/SVFIR.h"
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugProgramInstruction.h>
#include <algorithm>
#include <regex>
#include <set>
#include <utility>

using namespace SVF;
namespace ldc::frontend {
namespace detail {

std::unique_ptr<AndersenWaveDiff> runAndersen(SVFIR& pag) {
    using AnalysisKind = decltype(std::declval<AndersenWaveDiff>().getAnalysisTy());
    auto analysis =
        std::make_unique<AndersenWaveDiff>(&pag, AnalysisKind::AndersenWaveDiff_WPA, false);
    analysis->disablePrintStat();
    analysis->analyze();
    return analysis;
}

int sourceLine(const std::string& location) {
    static const std::regex lineRe(R"re("?ln"?\s*:\s*(\d+))re");
    std::smatch match;
    if (std::regex_search(location, match, lineRe)) return std::stoi(match[1].str());
    return 0;
}

std::unordered_map<SvfId, DebugName> debugNames() {
    std::unordered_map<SvfId, DebugName> names;
    LLVMModuleSet* modules = LLVMModuleSet::getLLVMModuleSet();
    auto record = [&](const llvm::Value* value, const std::string& function,
                      const std::string& name) {
        if (value && modules->hasValueNode(value))
            names.emplace(SvfId{modules->getValueNode(value)}, DebugName{function, name});
    };
    for (auto i = 0u; i < modules->getModuleNum(); ++i)
        for (const auto& fn : *modules->getModule(i)) {
            const auto function = llvm::demangle(fn.getName().str());
            for (const auto& arg : fn.args())
                if (arg.hasName()) record(&arg, function, arg.getName().str());
            for (const auto& block : fn)
                for (const auto& inst : block)
                    for (const llvm::DbgVariableRecord& dvr :
                         llvm::filterDbgVars(inst.getDbgRecordRange()))
                        if (dvr.getVariable() && dvr.getNumVariableLocationOps() == 1)
                            record(dvr.getVariableLocationOp(0), function,
                                   dvr.getVariable()->getName().str());
        }
    return names;
}

} // namespace detail

namespace {
using namespace detail;
class Builder {
public:
    Builder(SVFIR& pag, const CallGraph& calls, BuildStats& stats)
        : pag_(pag), calls_(calls), stats_(stats), names_(debugNames()) {}

    SimplifiedPAG run() {
        const auto statements = statementEdges(pag_);
        stats_.variantGeps = statements.variantGeps;
        add(statements.edges);
        const auto indirect = indirectCalls(pag_, calls_);
        add(indirect);
        stats_.indirectEdges = indirect.size();

        // Register all sites, including unresolved calls and calls with no data-flow edges.
        std::vector<const CallICFGNode*> sites(pag_.getCallSiteSet().begin(),
                                               pag_.getCallSiteSet().end());
        std::sort(sites.begin(), sites.end(),
                  [](const auto* a, const auto* b) { return a->getId() < b->getId(); });
        for (const auto* site : sites) callSite(site);
        return std::move(graph_);
    }

private:
    NodeId node(const SVFVar* value) {
        if (auto id = graph_.findSvf(SvfId{value->getId()})) return *id;
        Node info;
        info.svfId = SvfId{value->getId()};
        info.kind = SVFUtil::isa<ObjVar>(value) ? NodeKind::Obj : NodeKind::Var;
        info.name = value->getName();
        if (info.name.empty())
            info.name =
                (info.kind == NodeKind::Obj ? "o" : "v") + std::to_string(info.svfId->value);
        if (const auto* function = value->getFunction())
            info.function = llvm::demangle(function->getName());
        if (auto it = names_.find(*info.svfId); it != names_.end())
            info.sourceName = it->second.name;
        info.sourceLocation = value->getSourceLoc();
        info.line = sourceLine(info.sourceLocation);
        return graph_.addNode(info);
    }

    std::optional<NodeId> optionalNode(const SVFVar* value) {
        return relevant(value) ? std::optional<NodeId>(node(value)) : std::nullopt;
    }

    CallSiteId callSite(const CallICFGNode* site) {
        if (auto it = sites_.find(site); it != sites_.end()) return it->second;
        CallSite info;
        info.svfId = SvfId{site->getId()};
        info.caller = llvm::demangle(site->getCaller()->getName());
        info.sourceLocation = site->getSourceLoc();
        info.line = sourceLine(info.sourceLocation);
        info.flags.isIndirect = pag_.isIndirectCallSites(site);
        info.flags.isVirtual = site->isVirtualCall();
        for (const auto* actual : site->getActualParms())
            info.actuals.push_back(optionalNode(actual));
        info.actualRet = optionalNode(site->getRetICFGNode()->getActualRet());
        std::set<const FunObjVar*> targets;
        const auto& callEdges = calls_.getCallInstToCallGraphEdgesMap();
        if (auto it = callEdges.find(site); it != callEdges.end())
            for (const auto* edge : it->second) targets.insert(edge->getDstNode()->getFunction());
        for (const auto* callee : targets) {
            CallTarget target;
            target.function = llvm::demangle(callee->getName());
            if (pag_.hasFunArgsList(callee))
                for (const auto* formal : pag_.getFunArgsList(callee))
                    target.formals.push_back(optionalNode(formal));
            if (auto it = pag_.getFunRets().find(callee); it != pag_.getFunRets().end())
                target.ret = optionalNode(it->second);
            info.targets.push_back(std::move(target));
        }
        std::sort(info.targets.begin(), info.targets.end(),
                  [](const auto& a, const auto& b) { return a.function < b.function; });
        const auto id = graph_.addCallSite(info);
        sites_.emplace(site, id);
        return id;
    }

    void add(const SvfEdges& edges) {
        for (const auto& edge : edges) {
            const auto site =
                edge.site ? std::optional<CallSiteId>(callSite(edge.site)) : std::nullopt;
            if (!relevant(edge.src) || !relevant(edge.dst)) {
                ++stats_.skippedEdges;
                continue;
            }
            const auto src = node(edge.src);
            const auto dst = node(edge.dst);
            graph_.addEdge({src, dst, edge.label, edge.field, site, edge.dir});
        }
    }

    SVFIR& pag_;
    const CallGraph& calls_;
    BuildStats& stats_;
    SimplifiedPAG graph_;
    std::unordered_map<SvfId, DebugName> names_;
    std::unordered_map<const CallICFGNode*, CallSiteId> sites_;
};
} // namespace

SimplifiedPAG buildSimplifiedPAG(SVFIR& pag, const CallGraph& calls, BuildStats& stats) {
    stats = {};
    return Builder(pag, calls, stats).run();
}

void FrontendResult::printSummary(std::ostream& out) const {
    out << "SVFIR: " << svfNodeCount << " nodes\n";
    for (const auto& [caller, callee] : calls)
        out << "call: " << caller << " -> " << callee << "\n";
    graph.printSummary(out);
    out << "Build: " << stats.variantGeps << " variable-offset geps, " << stats.skippedEdges
        << " skipped edges, " << stats.indirectEdges << " indirect call/return edges\n";
}

FrontendResult analyzeModules(const std::vector<std::string>& modules) {
    if (modules.empty()) throw std::invalid_argument("No LLVM IR input modules");
    // Release in reverse dependency order, including when result construction throws.
    struct Session {
        ~Session() {
            SVFIR::releaseSVFIR();
            LLVMModuleSet::releaseLLVMModuleSet();
        }
    } session;
    LLVMModuleSet::buildSVFModule(modules);
    SVFIRBuilder builder;
    auto* pag = builder.build();
    auto andersen = detail::runAndersen(*pag);

    FrontendResult result;
    result.svfNodeCount = pag->getTotalNodeNum();
    std::set<std::pair<std::string, std::string>> calls;
    for (const auto& entry : *andersen->getCallGraph()) {
        const auto* caller = entry.second;
        for (const auto* edge : caller->getOutEdges())
            calls.emplace(caller->getName(), edge->getDstNode()->getName());
    }
    result.calls.assign(calls.begin(), calls.end());
    result.graph = buildSimplifiedPAG(*pag, *andersen->getCallGraph(), result.stats);
    return result;
}

} // namespace ldc::frontend
