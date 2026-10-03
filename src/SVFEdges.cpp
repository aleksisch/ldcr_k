#include "SVFDetails.h"
#include "Graphs/CallGraph.h"
#include "Graphs/ICFGNode.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFStatements.h"
#include "SVFIR/SVFVariables.h"

using namespace SVF;
namespace ldc::frontend::detail {
static bool isIntrinsicName(const std::string& name) { return name.rfind("llvm.", 0) == 0; }

/// Variables and objects that take part in pointer flow. Constants, dummies,
/// field objects (created by SVF's analyses, not by the program) and values
/// inside LLVM intrinsics are left out.
bool relevant(const SVFVar* var) {
    if (var == nullptr) return false;
    if (SVFUtil::isa<ConstDataValVar>(var) || SVFUtil::isa<ConstDataObjVar>(var) ||
        SVFUtil::isa<DummyValVar>(var) || SVFUtil::isa<DummyObjVar>(var) ||
        SVFUtil::isa<GepObjVar>(var))
        return false;
    if (const FunObjVar* fun = var->getFunction())
        if (isIntrinsicName(fun->getName())) return false;
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

// Resolve argument/return flow using Andersen's call graph.
IndirectCalls indirectCalls(SVFIR& pag, const CallGraph& callGraph, bool virtualCalls) {
    IndirectCalls calls;
    for (const auto& entry : callGraph)
        for (const auto* cgEdge : entry.second->getOutEdges()) {
            if (!cgEdge->isIndirectCallEdge()) continue;
            const FunObjVar* callee = cgEdge->getDstNode()->getFunction();
            for (const auto* cs : cgEdge->getIndirectCalls()) {
                calls.sites.push_back(cs);
                if (cs->isVirtualCall() && !virtualCalls) continue;
                if (pag.hasFunArgsList(callee)) {
                    const auto& formals = pag.getFunArgsList(callee);
                    const auto& actuals = cs->getActualParms();
                    for (std::size_t i = 0; i < formals.size() && i < actuals.size(); ++i)
                        calls.edges.push_back({actuals[i], formals[i], Label::Assign, std::nullopt,
                                               cs, CallDir::Enter});
                }
                auto ret = pag.getFunRets().find(callee);
                const SVFVar* actualRet = cs->getRetICFGNode()->getActualRet();
                if (ret != pag.getFunRets().end() && actualRet != nullptr)
                    calls.edges.push_back(
                        {ret->second, actualRet, Label::Assign, std::nullopt, cs, CallDir::Exit});
            }
        }
    return calls;
}

} // namespace ldc::frontend::detail
