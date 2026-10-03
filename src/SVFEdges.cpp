#include "SVFDetails.h"
#include "Graphs/CallGraph.h"
#include "Graphs/ICFGNode.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFStatements.h"
#include "SVFIR/SVFVariables.h"
#include "SVF-LLVM/LLVMModule.h"

using namespace SVF;
namespace ldc::frontend::detail {

/// Variables and objects that take part in pointer flow. Constants, dummies,
/// field objects (created by SVF's analyses, not by the program) and values
/// inside LLVM intrinsics are left out.
bool relevant(const SVFVar* var) {
    if (var == nullptr) return false;
    if (SVFUtil::isa<ConstDataValVar>(var) || SVFUtil::isa<ConstDataObjVar>(var) ||
        SVFUtil::isa<DummyValVar>(var) || SVFUtil::isa<DummyObjVar>(var) ||
        SVFUtil::isa<GepObjVar>(var))
        return false;
    if (const auto* fun = var->getFunction()) {
        auto* modules = LLVMModuleSet::getLLVMModuleSet();
        if (modules->hasLLVMValue(fun))
            if (const auto* function = llvm::dyn_cast<llvm::Function>(modules->getLLVMValue(fun)))
                if (function->isIntrinsic()) return false;
    }
    return true;
}

// Preserve each GEP and the original memory-access endpoints.
StatementEdges statementEdges(SVFIR& pag) {
    StatementEdges result;
    auto& edges = result.edges;
    for (const auto* stmt : pag.getSVFStmtSet(SVFStmt::Gep)) {
        const auto* gep = SVFUtil::cast<GepStmt>(stmt);
        auto field = kAnyField;
        if (gep->isVariantFieldGep())
            ++result.variantGeps;
        else
            field = FieldOffset{static_cast<std::int32_t>(gep->getConstantStructFldIdx())};
        edges.push_back({gep->getRHSVar(), gep->getLHSVar(), Label::Gep, field});
    }
    auto stmts = [&](SVFStmt::PEDGEK kind) { return pag.getSVFStmtSet(kind); };
    for (const auto* s : stmts(SVFStmt::Addr))
        edges.push_back({s->getSrcNode(), s->getDstNode(), Label::New});
    for (const auto* s : stmts(SVFStmt::Copy))
        edges.push_back({s->getSrcNode(), s->getDstNode(), Label::Assign});
    for (auto kind : {SVFStmt::Phi, SVFStmt::Select})
        for (const auto* s : stmts(kind))
            for (const auto* op : SVFUtil::cast<MultiOpndStmt>(s)->getOpndVars())
                edges.push_back({op, s->getDstNode(), Label::Assign});
    for (const auto* s : stmts(SVFStmt::Store))
        edges.push_back({s->getSrcNode(), s->getDstNode(), Label::Store, FieldOffset{0}});
    for (const auto* s : stmts(SVFStmt::Load))
        edges.push_back({s->getSrcNode(), s->getDstNode(), Label::Load, FieldOffset{0}});
    for (const auto* s : stmts(SVFStmt::Call)) { // formal ← one actual per call site
        const auto* call = SVFUtil::cast<CallPE>(s);
        for (auto i = 0u; i < call->getOpVarNum(); ++i)
            edges.push_back({call->getOpVar(i), call->getRes(), Label::Assign, std::nullopt,
                             call->getOpCallICFGNode(i), CallDir::Enter});
    }
    for (const auto* s : stmts(SVFStmt::Ret)) { // formal return → actual return
        const auto* ret = SVFUtil::cast<RetPE>(s);
        edges.push_back({ret->getRHSVar(), ret->getLHSVar(), Label::Assign, std::nullopt,
                         ret->getCallInst(), CallDir::Exit});
    }
    return result;
}

// Resolve argument/return flow of indirect calls using Andersen's call graph.
SvfEdges indirectCalls(SVFIR& pag, const CallGraph& callGraph) {
    SvfEdges edges;
    for (const auto& [cs, cgEdges] : callGraph.getCallInstToCallGraphEdgesMap()) {
        if (!cs->isIndirectCall()) continue;
        const auto& actuals = cs->getActualParms();
        const auto* actualRet = cs->getRetICFGNode()->getActualRet();
        for (const auto* cgEdge : cgEdges) {
            const auto* callee = cgEdge->getDstNode()->getFunction();
            if (pag.hasFunArgsList(callee)) {
                const auto& formals = pag.getFunArgsList(callee);
                for (std::size_t i = 0; i < formals.size() && i < actuals.size(); ++i) {
                    edges.push_back(
                        {actuals[i], formals[i], Label::Assign, std::nullopt, cs, CallDir::Enter});
                }
            }
            const auto ret = pag.getFunRets().find(callee);
            if (actualRet && ret != pag.getFunRets().end()) {
                edges.push_back(
                    {ret->second, actualRet, Label::Assign, std::nullopt, cs, CallDir::Exit});
            }
        }
    }
    return edges;
}

} // namespace ldc::frontend::detail
