#include "GEPFolding.h"
#include "Graphs/ICFGNode.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFStatements.h"
#include <unordered_map>
#include <unordered_set>

using namespace SVF;
namespace ldc::frontend::detail {
namespace {
struct Geps {
    std::unordered_map<SvfId, std::pair<const SVFVar*, FieldOffset>> def; ///< result -> (base, f)
    std::size_t variant = 0; ///< geps with a variable struct field (field `*`)
};

Geps collectGeps(SVFIR& pag) {
    Geps geps;
    for (const auto* stmt : pag.getSVFStmtSet(SVFStmt::Gep)) {
        const auto* gep = SVFUtil::cast<GepStmt>(stmt);
        // As SVF: array indices are ignored; only arithmetic over struct fields loses the field.
        FieldOffset field = kAnyField;
        if (!gep->isVariantFieldGep())
            field = FieldOffset{static_cast<std::int32_t>(gep->getConstantStructFldIdx())};
        else
            ++geps.variant;
        geps.def[SvfId{gep->getLHSVarID()}] = {gep->getRHSVar(), field};
    }
    return geps;
}

/// Resolves an address to (base pointer, field), following chains of geps.
std::pair<const SVFVar*, FieldOffset> resolveAddress(const Geps& geps, const SVFVar* ptr) {
    FieldOffset field{0};
    const SVFVar* base = ptr;
    std::unordered_set<SvfId> visited;
    for (auto it = geps.def.find(SvfId{base->getId()}); it != geps.def.end();
         it = geps.def.find(SvfId{base->getId()})) {
        if (!visited.insert(SvfId{base->getId()}).second) return {base, kAnyField};
        const FieldOffset step = it->second.second;
        field = (field == kAnyField || step == kAnyField) ? kAnyField
                                                          : FieldOffset{field.value + step.value};
        base = it->second.first;
    }
    return {base, field};
}

/// Gep results used as a value: stored, copied, passed, returned, merged by phi/select.
std::unordered_set<SvfId> findEscapingGeps(SVFIR& pag, const Geps& geps) {
    std::unordered_set<SvfId> escaping;
    auto check = [&](const SVFVar* var) {
        if (var != nullptr && geps.def.count(SvfId{var->getId()}) != 0)
            escaping.insert(SvfId{var->getId()});
    };
    for (const auto* stmt : pag.getSVFStmtSet(SVFStmt::Copy))
        check(SVFUtil::cast<CopyStmt>(stmt)->getRHSVar());
    for (const auto* stmt : pag.getSVFStmtSet(SVFStmt::Store))
        check(SVFUtil::cast<StoreStmt>(stmt)->getRHSVar()); // stored value, not the address
    for (const auto* stmt : pag.getSVFStmtSet(SVFStmt::Call))
        for (const auto* op : SVFUtil::cast<CallPE>(stmt)->getOpndVars()) check(op);
    // Indirect calls do not necessarily have CallPE statements in SVFIR.
    for (const auto* site : pag.getCallSiteSet())
        for (const auto* actual : site->getActualParms()) check(actual);
    for (const auto* stmt : pag.getSVFStmtSet(SVFStmt::Ret))
        check(SVFUtil::cast<RetPE>(stmt)->getRHSVar()); // returned
    for (const auto& entry : pag.getFunRets())
        check(entry.second); // returned (also from functions without callers)
    for (const auto* stmt : pag.getSVFStmtSet(SVFStmt::Phi))
        for (const auto* op : SVFUtil::cast<PhiStmt>(stmt)->getOpndVars()) check(op);
    for (const auto* stmt : pag.getSVFStmtSet(SVFStmt::Select))
        for (const auto* op : SVFUtil::cast<SelectStmt>(stmt)->getOpndVars()) check(op);
    return escaping;
}

} // namespace

SvfEdges foldGeps(SVFIR& pag, const SvfEdges& edges) {
    const auto geps = collectGeps(pag);
    const auto escaping = findEscapingGeps(pag, geps);
    SvfEdges folded;
    for (auto id : escaping) {
        const auto* gep = pag.getGNode(id.value);
        const auto [base, field] = resolveAddress(geps, gep);
        folded.push_back({base, gep, Label::Gep, field});
    }
    for (auto edge : edges) {
        if (edge.label == Label::Gep) continue;
        if (edge.label == Label::Store) {
            const auto [base, field] = resolveAddress(geps, edge.dst);
            edge.dst = base;
            edge.field = field;
        } else if (edge.label == Label::Load) {
            const auto [base, field] = resolveAddress(geps, edge.src);
            edge.src = base;
            edge.field = field;
        }
        folded.push_back(edge);
    }
    return folded;
}

} // namespace ldc::frontend::detail
