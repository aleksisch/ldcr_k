#include "SVFDetails.h"
#include "Graphs/CallGraph.h"
#include "Graphs/ICFGNode.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFStatements.h"
#include "SVFIR/SVFVariables.h"
#include <unordered_set>
#include <unordered_map>
#include <utility>

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

// Fold `q = gep p, f; *q = v` into `v --store[f]--> p`. Loads
// and stores through a gep result are folded onto the gep's base (a plain `*p` is field 0).

namespace {
struct Geps {
    std::unordered_map<SvfId, std::pair<const SVFVar*, FieldId>> def; ///< result -> (base, f)
    std::size_t variant = 0; ///< geps with a variable struct field (field `*`)
};

Geps collectGeps(SVFIR& pag) {
    Geps geps;
    for (const SVFStmt* stmt : pag.getSVFStmtSet(SVFStmt::Gep)) {
        const auto* gep = SVFUtil::cast<GepStmt>(stmt);
        // As SVF: array indices are ignored; only arithmetic over struct fields loses the field.
        FieldId field = kAnyField;
        if (!gep->isVariantFieldGep())
            field = static_cast<FieldId>(gep->getConstantStructFldIdx());
        else
            ++geps.variant;
        geps.def[gep->getLHSVarID()] = {gep->getRHSVar(), field};
    }
    return geps;
}

/// Resolves an address to (base pointer, field), following chains of geps.
std::pair<const SVFVar*, FieldId> resolveAddress(const Geps& geps, const SVFVar* ptr) {
    FieldId field = 0;
    const SVFVar* base = ptr;
    std::unordered_set<SvfId> visited;
    for (auto it = geps.def.find(base->getId()); it != geps.def.end();
         it = geps.def.find(base->getId())) {
        if (!visited.insert(base->getId()).second) return {base, kAnyField};
        const FieldId step = it->second.second;
        field = (field == kAnyField || step == kAnyField) ? kAnyField : field + step;
        base = it->second.first;
    }
    return {base, field};
}

/// Gep results used as a value: stored, copied, passed, returned, merged by phi/select.
std::unordered_set<SvfId> findEscapingGeps(SVFIR& pag, const Geps& geps) {
    std::unordered_set<SvfId> escaping;
    auto check = [&](const SVFVar* var) {
        if (var != nullptr && geps.def.count(var->getId()) != 0) escaping.insert(var->getId());
    };
    for (const SVFStmt* stmt : pag.getSVFStmtSet(SVFStmt::Copy))
        check(SVFUtil::cast<CopyStmt>(stmt)->getRHSVar());
    for (const SVFStmt* stmt : pag.getSVFStmtSet(SVFStmt::Store))
        check(SVFUtil::cast<StoreStmt>(stmt)->getRHSVar()); // stored value, not the address
    for (const SVFStmt* stmt : pag.getSVFStmtSet(SVFStmt::Call))
        for (const ValVar* op : SVFUtil::cast<CallPE>(stmt)->getOpndVars()) check(op);
    // Indirect calls do not necessarily have CallPE statements in SVFIR.
    for (const auto* site : pag.getCallSiteSet())
        for (const auto* actual : site->getActualParms()) check(actual);
    for (const SVFStmt* stmt : pag.getSVFStmtSet(SVFStmt::Ret))
        check(SVFUtil::cast<RetPE>(stmt)->getRHSVar()); // returned
    for (const auto& entry : pag.getFunRets())
        check(entry.second); // returned (also from functions without callers)
    for (const SVFStmt* stmt : pag.getSVFStmtSet(SVFStmt::Phi))
        for (const ValVar* op : SVFUtil::cast<PhiStmt>(stmt)->getOpndVars()) check(op);
    for (const SVFStmt* stmt : pag.getSVFStmtSet(SVFStmt::Select))
        for (const ValVar* op : SVFUtil::cast<SelectStmt>(stmt)->getOpndVars()) check(op);
    return escaping;
}

/// The edges of SVFIR's statements. An escaping gep (the vptr `&vtable[2]` stored by
/// constructors, `&obj->member` passed as `this`, ...) becomes `base --gep[f]--> q`: q points to
/// the field objects ⟨O, off + f⟩, like SVF's GepObjVar.
SvfEdges statementEdges(SVFIR& pag, const Geps& geps, const std::unordered_set<SvfId>& escaping) {
    SvfEdges edges;
    for (SvfId id : escaping) {
        const SVFVar* gep = pag.getGNode(id);
        const auto [base, field] = resolveAddress(geps, gep);
        edges.push_back({base, gep, Label::Gep, field});
    }
    auto stmts = [&](SVFStmt::PEDGEK kind) { return pag.getSVFStmtSet(kind); };
    for (const SVFStmt* s : stmts(SVFStmt::Addr))
        edges.push_back({s->getSrcNode(), s->getDstNode(), Label::New});
    for (const SVFStmt* s : stmts(SVFStmt::Copy))
        edges.push_back({s->getSrcNode(), s->getDstNode(), Label::Assign});
    for (SVFStmt::PEDGEK kind : {SVFStmt::Phi, SVFStmt::Select})
        for (const SVFStmt* s : stmts(kind))
            for (const ValVar* op : SVFUtil::cast<MultiOpndStmt>(s)->getOpndVars())
                edges.push_back({op, s->getDstNode(), Label::Assign});
    for (const SVFStmt* s : stmts(SVFStmt::Store)) { // *p = v  →  v --store[f]--> base(p)
        auto [base, field] = resolveAddress(geps, s->getDstNode());
        edges.push_back({s->getSrcNode(), base, Label::Store, field});
    }
    for (const SVFStmt* s : stmts(SVFStmt::Load)) { // x = *p  →  base(p) --load[f]--> x
        auto [base, field] = resolveAddress(geps, s->getSrcNode());
        edges.push_back({base, s->getDstNode(), Label::Load, field});
    }
    for (const SVFStmt* s : stmts(SVFStmt::Call)) { // formal ← one actual per call site
        const auto* call = SVFUtil::cast<CallPE>(s);
        for (u32_t i = 0; i < call->getOpVarNum(); ++i)
            edges.push_back({call->getOpVar(i), call->getRes(), Label::Assign, std::nullopt,
                             call->getOpCallICFGNode(i), CallDir::Enter});
    }
    for (const SVFStmt* s : stmts(SVFStmt::Ret)) { // formal return → actual return
        const auto* ret = SVFUtil::cast<RetPE>(s);
        edges.push_back({ret->getRHSVar(), ret->getLHSVar(), Label::Assign, std::nullopt,
                         ret->getCallInst(), CallDir::Exit});
    }
    return edges;
}

} // namespace

// Resolve argument/return flow using Andersen's call graph.
IndirectCalls indirectCalls(SVFIR& pag, const CallGraph& callGraph, bool virtualCalls) {
    IndirectCalls calls;
    for (const auto& entry : callGraph)
        for (const CallGraphEdge* cgEdge : entry.second->getOutEdges()) {
            if (!cgEdge->isIndirectCallEdge()) continue;
            const FunObjVar* callee = cgEdge->getDstNode()->getFunction();
            for (const CallICFGNode* cs : cgEdge->getIndirectCalls()) {
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

StatementEdges statementEdges(SVFIR& pag) {
    const Geps geps = collectGeps(pag);
    return {statementEdges(pag, geps, findEscapingGeps(pag, geps)), geps.variant};
}

} // namespace ldc::frontend::detail
