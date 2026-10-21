#include "ldc/Grammar.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ldc::grammar {

namespace {

using frontend::Label;
using frontend::SimplifiedPAG;

struct Rules {
    cfl::Grammar grammar;
    void operator()(const std::string& lhs, const std::string& a, const std::string& b = "") {
        grammar.push_back({lhs, a, b});
    }
};

// The classes of objects: the types of allocations (nullopt: untyped), and the types dispatched on.
struct Classes {
    std::set<std::optional<std::uint32_t>> types;
    std::set<std::uint32_t> dispatched;
};

Classes classesOf(const SimplifiedPAG& pag) {
    Classes classes;
    for (const auto& e : pag.edges()) {
        if (e.label == Label::New)
            classes.types.insert(e.type ? std::optional(e.type->value) : std::nullopt);
        if (e.label == Label::Dispatch) classes.dispatched.insert(e.type->value);
    }
    return classes;
}

// A class's type in terminal names ("" if untyped) and in nonterminal names.
std::string typeName(const std::optional<std::uint32_t>& type) {
    return type ? std::to_string(*type) : "";
}

std::string className(const std::optional<std::uint32_t>& type) {
    return type ? "t" + std::to_string(*type) : "u";
}

// flowsto per class c, so that dispatch[t] admits only objects of type t: FT_c(o, x) holds if
// object o of class c flows to x through allocations, assignments, dispatches and `heap`, a flow
// through the heap; FTR_c is its reverse (over the reversed edges, "_r"); ALIAS(b, c) holds if b
// and c point to the same object. Calls and returns are assignments here (L_C matches them).
void flowsto(Rules& rule, const Classes& classes, const std::string& heap) {
    for (const auto& type : classes.types) {
        const auto n = typeName(type);
        const auto ft = "FT" + className(type), ftr = "FTR" + className(type);
        rule("S", ft);
        rule(ft, "new" + n);
        rule(ftr, "new" + n + "_r");
        rule(ft, ft, "assign");
        rule(ftr, "assign_r", ftr);
        rule(ft, ft, heap);
        rule(ftr, heap + "R", ftr);
        if (type && classes.dispatched.count(*type)) {
            rule(ft, ft, "dispatch" + n);
            rule(ftr, "dispatch" + n + "_r", ftr);
        }
        rule("ALIAS", ftr, ft);
    }
}

// A dispatch site's synthetic argument and result fields: storeArg alias loadArg.
void syntheticFields(Rules& rule, const std::string& heap) {
    rule(heap, "SSA_i", "loadArg_i");
    rule("SSA_i", "storeArg_i", "ALIAS");
    rule(heap + "R", "SLA_i", "storeArg_r_i");
    rule("SLA_i", "loadArg_r_i", "ALIAS");
}

} // namespace

// The paper's eq. 8:
//
//   flowsto  -> new[t] (flows | dispatch[t])*      FT_c
//   flows    -> assign | store[f] alias load[f]    FLOWS(v, w): a value flows through the heap
//   alias    -> flowsto~ flowsto                   ALIAS
//
// Extensions for C++: a pointer that took the address of a field (gep) is interior (IFT_c,
// IFTR_c), and its stores and loads may touch any field of its object (ALIASI); a store or a load
// through an unknown field (storeAny, loadAny) matches every field; the synthetic argument and
// result fields of a dispatch site (storeArg, loadArg) match like fields.
cfl::Grammar ld(const SimplifiedPAG& pag) {
    const auto classes = classesOf(pag);
    Rules rule;
    flowsto(rule, classes, "FLOWS");
    for (const auto& type : classes.types) {
        const auto c = className(type);
        const auto ft = "FT" + c, ftr = "FTR" + c, ift = "IFT" + c, iftr = "IFTR" + c;
        rule("S", ift);
        rule(ift, ft, "gep_i");
        rule(iftr, "gep_r_i", ftr);
        rule(ift, ift, "gep_i");
        rule(iftr, "gep_r_i", iftr);
        rule(ift, ift, "assign");
        rule(iftr, "assign_r", iftr);
        rule(ift, ift, "FLOWS");
        rule(iftr, "FLOWSR", iftr);
        rule("ALIASI", iftr, ft);
        rule("ALIASI", ftr, ift);
        rule("ALIASI", iftr, ift);
    }
    rule("ALIASX", "ALIAS");
    rule("ALIASX", "ALIASI");
    rule("STX", "store_i");
    rule("STX", "storeAny");
    rule("LDX", "load_i");
    rule("LDX", "loadAny");
    rule("STXR", "store_r_i");
    rule("STXR", "storeAny_r");
    rule("LDXR", "load_r_i");
    rule("LDXR", "loadAny_r");

    // store[f] alias load[f]: the same field through the same object, any field through an
    // interior pointer or an unknown field.
    rule("FLOWS", "SA_i", "load_i");
    rule("SA_i", "store_i", "ALIAS");
    rule("FLOWS", "SAI", "LDX");
    rule("SAI", "STX", "ALIASI");
    rule("FLOWS", "SAS", "LDX");
    rule("SAS", "storeAny", "ALIASX");
    rule("FLOWS", "SAX", "loadAny");
    rule("SAX", "STX", "ALIASX");

    // The same backwards: load~ alias store~.
    rule("FLOWSR", "LA_i", "store_r_i");
    rule("LA_i", "load_r_i", "ALIAS");
    rule("FLOWSR", "LAI", "STXR");
    rule("LAI", "LDXR", "ALIASI");
    rule("FLOWSR", "LAS", "STXR");
    rule("LAS", "loadAny_r", "ALIASX");
    rule("FLOWSR", "LAX", "storeAny_r");
    rule("LAX", "LDXR", "ALIASX");
    syntheticFields(rule, "FLOWS");
    return rule.grammar;
}

// L_D over addObjectFields' PAG: an object at an offset (class u) flows to w if the object at the
// offset before the gep flows to the gep's source (GPO); a store and a load meet in a heap cell
// (o, a + f) of the object both bases point to (HEAP), an unknown field through the object's star
// cell or its hub.
cfl::Grammar ldCpp(const SimplifiedPAG& pag) {
    auto classes = classesOf(pag);
    classes.types.insert(std::nullopt);
    Rules rule;
    flowsto(rule, classes, "HEAP");
    for (const auto& type : classes.types) {
        const auto c = className(type), shift = "shift" + typeName(type);
        rule("GPO" + c + "_i", "FT" + c, "gep_i");
        rule("GPR" + c + "_i", "gep_r_i", "FTR" + c);
        rule("FTu", shift + "_i", "GPO" + c + "_i");
        rule("FTRu", "GPR" + c + "_i", shift + "_r_i");
    }
    rule("HEAP", "STCELL", "LDCELL");
    rule("STCELL", "STO_i", "cell_r_i");
    rule("STCELL", "STOS", "starCellAny_r");
    rule("LDCELL", "cell_i", "LDO_i");
    rule("LDCELL", "starCell_i", "LDO_i");
    rule("LDCELL", "cellHub", "HUBLD");
    rule("HUBLD", "hub_i", "LDO_i");
    rule("HUBLD", "hubAny", "LDOS");
    rule("HEAPR", "LDCELLR", "STCELLR");
    rule("STCELLR", "cell_i", "STOR_i");
    rule("STCELLR", "starCellAny", "STORS");
    rule("LDCELLR", "LDOR_i", "cell_r_i");
    rule("LDCELLR", "LDOR_i", "starCell_r_i");
    rule("LDCELLR", "HUBLDR", "cellHub_r");
    rule("HUBLDR", "LDOR_i", "hub_r_i");
    rule("HUBLDR", "LDORS", "hubAny_r");
    for (const auto& type : classes.types) {
        const auto ft = "FT" + className(type), ftr = "FTR" + className(type);
        rule("STO_i", "store_i", ftr);
        rule("STOS", "storeAny", ftr);
        rule("LDO_i", ft, "load_i");
        rule("LDOS", ft, "loadAny");
        rule("STOR_i", ft, "store_r_i");
        rule("STORS", ft, "storeAny_r");
        rule("LDOR_i", "load_r_i", ftr);
        rule("LDORS", "loadAny_r", ftr);
    }
    syntheticFields(rule, "HEAP");
    return rule.grammar;
}

cfl::Grammar lc() {
    // A realizable path (S) has unmatched returns first (P), then unmatched calls (R); M is
    // balanced. Dispatch boundaries are plain steps (E) for L_C.
    return {
        {"S", "P"},           {"S", "R"},       {"E", "e"},         {"E", "bo"},
        {"E", "bc"},          {"M", "E"},       {"M", "M", "M"},    {"M", "OP_i", "T_i"},
        {"T_i", "M", "CL_i"}, {"T_i", "ret_i"}, {"OP_i", "call_i"}, {"CL_i", "ret_i"},
        {"OPX", "call_i"},    {"CLX", "ret_i"}, {"P", "E"},         {"P", "CLX"},
        {"P", "OP_i", "T_i"}, {"P", "P", "M"},  {"P", "P", "CLX"},  {"R", "P", "OPX"},
        {"R", "R", "OPX"},    {"R", "R", "M"},
    };
}

cfl::Grammar lr() {
    // The paper's eq. 17. Outside a dispatch excursion anything goes (S). An excursion opened at
    // site c (bo_c) closes at c (bc_c), and inside it every climb out of a call (ret_k) is undone
    // by going back in through the same site (call_k, IN), with calls into and back out of other
    // functions (MS) and nested excursions in between.
    return {
        {"S", "STEP"},
        {"S", "S", "STEP"},
        {"STEP", "e"},
        {"STEP", "OPX"},
        {"STEP", "CLX"},
        {"STEP", "SITE"},
        {"OPX", "call_i"},
        {"CLX", "ret_i"},
        {"SITE", "bo_i", "U_i"},
        {"U_i", "IN", "bc_i"},
        {"U_i", "bc_i"},
        {"IN", "INU"},
        {"IN", "IN", "INU"},
        {"INU", "MU"},
        {"INU", "CLIMB"},
        {"CLIMB", "ret_i", "V_i"},
        {"V_i", "IN", "call_i"},
        {"V_i", "call_i"},
        {"MU", "e"},
        {"MU", "SITE"},
        {"MU", "call_i", "W_i"},
        {"W_i", "MS", "ret_i"},
        {"W_i", "ret_i"},
        {"MS", "MU"},
        {"MS", "MS", "MU"},
    };
}

} // namespace ldc::grammar
