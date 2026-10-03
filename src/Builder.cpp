#include "ldc/Builder.h"

#include "Graphs/CallGraph.h"
#include "Graphs/ICFGNode.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFStatements.h"
#include "SVFIR/SVFVariables.h"
#include "SVF-LLVM/LLVMModule.h"

#include <llvm/Demangle/Demangle.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include <optional>
#include <set>
#include <vector>

#include <ostream>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using namespace SVF;

namespace ldc {

void BuildStats::print(std::ostream& os) const {
    os << "Build: " << variantGeps << " variable-offset geps, " << skippedStmts
       << " skipped statements, " << indirectEdges << " indirect call/return edges from Andersen, "
       << sitesWithoutDeclaredType << " virtual call sites without a declared class\n";
}

namespace {

int parseLine(const std::string& sourceLoc) {
    static const std::regex lineRe(R"re("?ln"?\s*:\s*(\d+))re");
    std::smatch m;
    if (std::regex_search(sourceLoc, m, lineRe)) return std::stoi(m[1].str());
    return 0;
}

bool isIntrinsicName(const std::string& name) { return name.rfind("llvm.", 0) == 0; }

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

// The functions below only read SVF (whose getters are not const) and return their result;
// the Builder at the end is the only place that writes the graph.

/// An LDGraph edge between SVF variables, before its nodes and call site exist in the graph.
struct SvfEdge {
    const SVFVar* src;
    const SVFVar* dst;
    Label label;
    FieldId field = kNoField;
    const CallICFGNode* site = nullptr;
    CallDir dir = CallDir::None;
};
using SvfEdges = std::vector<SvfEdge>;

// SVFIR models `p->f = v` as `q = gep p, f; *q = v`; L_D wants `v --store[f]--> p`, so loads
// and stores through a gep result are folded onto the gep's base (a plain `*p` is field 0).

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
    for (auto it = geps.def.find(base->getId()); it != geps.def.end();
         it = geps.def.find(base->getId())) {
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
            edges.push_back({call->getOpVar(i), call->getRes(), Label::Assign, kNoField,
                             call->getOpCallICFGNode(i), CallDir::Enter});
    }
    for (const SVFStmt* s : stmts(SVFStmt::Ret)) { // formal return → actual return
        const auto* ret = SVFUtil::cast<RetPE>(s);
        edges.push_back({ret->getRHSVar(), ret->getLHSVar(), Label::Assign, kNoField,
                         ret->getCallInst(), CallDir::Exit});
    }
    return edges;
}

struct IndirectCalls {
    std::vector<const CallICFGNode*> sites; ///< one per (site, callee) in the call graph
    SvfEdges edges;
};

/// SVFIR has no CallPE/RetPE for indirect (incl. virtual) call sites: SVF's Andersen
/// connects them only in its own constraint graph. Wire them from the resolved call graph —
/// the L_FC way ([P-VCall]). Virtual call sites only with `virtualCalls` (Lfc).
IndirectCalls indirectCalls(SVFIR& pag, const CallGraph& callGraph, bool virtualCalls) {
    IndirectCalls calls;
    for (const auto& entry : callGraph)
        for (const CallGraphEdge* cgEdge : entry.second->getOutEdges()) {
            if (!cgEdge->isIndirectCallEdge()) continue;
            const FunObjVar* callee = cgEdge->getDstNode()->getFunction();
            for (const CallICFGNode* cs : cgEdge->getIndirectCalls()) {
                calls.sites.push_back(cs);
                if (cs->isVirtualCall() && !virtualCalls)
                    continue; // L_D and kCFA dispatch by the receiver's type instead
                if (pag.hasFunArgsList(callee)) {
                    const auto& formals = pag.getFunArgsList(callee);
                    const auto& actuals = cs->getActualParms();
                    for (std::size_t i = 0; i < formals.size() && i < actuals.size(); ++i)
                        calls.edges.push_back(
                            {actuals[i], formals[i], Label::Assign, kNoField, cs, CallDir::Enter});
                }
                auto ret = pag.getFunRets().find(callee);
                const SVFVar* actualRet = cs->getRetICFGNode()->getActualRet();
                if (ret != pag.getFunRets().end() && actualRet != nullptr)
                    calls.edges.push_back(
                        {ret->second, actualRet, Label::Assign, kNoField, cs, CallDir::Exit});
            }
        }
    return calls;
}

std::string stripPrefix(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0 ? text.substr(prefix.size()) : "";
}

/// DeclTypeOf(r) of a virtual call, from clang's `llvm.public.type.test(%vtable, !"_ZTS<class>")`
/// (-fwhole-program-vtables); SVF's getFunNameOfVirtualCall() is empty without its preprocessing.
std::string declaredClass(const CallICFGNode* cs) {
    const llvm::Value* vtable = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(cs->getVtablePtr());
    if (vtable == nullptr) return "";
    for (const llvm::User* user : vtable->users()) {
        const auto* call = llvm::dyn_cast<llvm::CallBase>(user);
        const llvm::Function* callee = call ? call->getCalledFunction() : nullptr;
        if (callee == nullptr ||
            (callee->getName() != "llvm.public.type.test" && callee->getName() != "llvm.type.test"))
            continue;
        if (const auto* md = llvm::dyn_cast<llvm::MetadataAsValue>(call->getArgOperand(1)))
            if (const auto* id = llvm::dyn_cast<llvm::MDString>(md->getMetadata()))
                return stripPrefix(llvm::demangle(id->getString().str()), "typeinfo name for ");
    }
    return "";
}

std::string vtableClass(const llvm::GlobalVariable* vtable) {
    return stripPrefix(llvm::demangle(vtable->getName().str()), "vtable for ");
}

/// Class name of a C++ debug-info type, from its ODR identifier (_ZTS...).
std::string debugClass(const llvm::DICompositeType* type) {
    if (type == nullptr || type->getIdentifier().empty()) return "";
    return stripPrefix(llvm::demangle(type->getIdentifier().str()), "typeinfo name for ");
}

using Hierarchy =
    std::unordered_map<std::string, std::vector<std::string>>; ///< class -> direct bases

Hierarchy collectHierarchy() {
    Hierarchy hierarchy;
    LLVMModuleSet* modules = LLVMModuleSet::getLLVMModuleSet();
    for (u32_t i = 0; i < modules->getModuleNum(); ++i) {
        llvm::DebugInfoFinder finder;
        finder.processModule(*modules->getModule(i));
        for (const llvm::DIType* type : finder.types()) {
            const auto* composite = llvm::dyn_cast<llvm::DICompositeType>(type);
            const std::string cls = debugClass(composite);
            if (cls.empty()) continue;
            auto& bases = hierarchy[cls];
            for (const llvm::DINode* element : composite->getElements()) {
                const auto* derived = llvm::dyn_cast<llvm::DIDerivedType>(element);
                if (derived == nullptr || derived->getTag() != llvm::dwarf::DW_TAG_inheritance)
                    continue;
                const std::string base = debugClass(
                    llvm::dyn_cast_or_null<llvm::DICompositeType>(derived->getBaseType()));
                if (!base.empty()) bases.push_back(base);
            }
        }
    }
    return hierarchy;
}

bool derives(const Hierarchy& hierarchy, const std::string& cls, const std::string& base) {
    if (cls == base) return true;
    auto it = hierarchy.find(cls);
    if (it == hierarchy.end()) return false;
    for (const std::string& direct : it->second)
        if (derives(hierarchy, direct, base)) return true;
    return false;
}

/// A vtable of the program: its LLVM global and its SVF object node.
struct VTable {
    const llvm::GlobalVariable* global;
    const SVFVar* object;
};

/// All vtables with an initializer (Itanium ABI: globals named _ZTV...). Taken from LLVM,
/// then mapped to SVF objects: SVF names these objects after the class ("A"), not "_ZTV1A".
std::vector<VTable> collectVTables(SVFIR& pag) {
    std::vector<VTable> vtables;
    LLVMModuleSet* modules = LLVMModuleSet::getLLVMModuleSet();
    for (u32_t i = 0; i < modules->getModuleNum(); ++i)
        for (const llvm::GlobalVariable& global : modules->getModule(i)->globals())
            if (global.getName().starts_with("_ZTV") && global.hasInitializer())
                vtables.push_back({&global, pag.getGNode(modules->getObjectNode(&global))});
    return vtables;
}

/// The function in `slot` of a vtable (Itanium ABI, single inheritance: the vptr points at
/// element 2 of `[offset-to-top, RTTI, f0, f1, ...]`).
const llvm::Function* vtableSlot(const llvm::GlobalVariable* vtable, s32_t slot) {
    constexpr unsigned kAddressPoint = 2;
    if (slot < 0) return nullptr;
    const llvm::Constant* array = vtable->getInitializer()->getAggregateElement(0u);
    if (array == nullptr) return nullptr;
    const llvm::Constant* entry =
        array->getAggregateElement(kAddressPoint + static_cast<unsigned>(slot));
    return entry ? llvm::dyn_cast<llvm::Function>(entry->stripPointerCasts()) : nullptr;
}

/// A virtual call site and its CHA targets.
struct VirtualSite {
    const CallICFGNode* cs;
    bool declared; ///< DeclTypeOf(r) is known
    /// The method in the call's slot of every vtable that may dispatch here, in vtable order.
    std::vector<std::pair<const llvm::Function*, const SVFVar*>> slots;
};

/// Every virtual call site with its CHA targets: the methods in the call's vtable slot, over
/// all vtables of classes t <: DeclTypeOf(r) ([C-VCall]; unknown hierarchy: no filter). SVF's
/// CHG (getVFnsFromVtbls) returns nothing in this SVF build, so the vtables are read directly
/// (vtableSlot). L_D's dispatch[t] then filters by the receiver's type.
std::vector<VirtualSite> virtualSites(SVFIR& pag, const std::vector<VTable>& vtables,
                                      const Hierarchy& hierarchy) {
    std::vector<VirtualSite> sites;
    for (const CallICFGNode* cs : pag.getCallSiteSet()) {
        if (!cs->isVirtualCall() || isIntrinsicName(cs->getCaller()->getName())) continue;
        const std::string declared = declaredClass(cs);
        VirtualSite site{cs, !declared.empty(), {}};
        for (const VTable& vtable : vtables) {
            if (!declared.empty() && hierarchy.count(declared) != 0 &&
                !derives(hierarchy, vtableClass(vtable.global), declared))
                continue;
            const llvm::Function* fn = vtableSlot(vtable.global, cs->getFunIdxInVtable());
            if (fn != nullptr && !fn->isDeclaration() && relevant(vtable.object))
                site.slots.push_back({fn, vtable.object});
        }
        sites.push_back(std::move(site));
    }
    return sites;
}

// Dynamic types (PLAN.md §5, §6): an object gets the class of the constructor called directly
// on its allocation ([C-New]); placement new gives each construction site its own object;
// `T::T(this + off)` in C's constructor types the member subobject ⟨O, off⟩ of every C object.

/// "ns::X::X(int)" -> "ns::X", "ns::T<1>::T()" -> "ns::T<1>"; empty if `function` is not a
/// constructor.
std::string constructorClass(const std::string& function) {
    const std::string qualified = function.substr(0, function.find('('));
    const std::size_t sep = qualified.rfind("::");
    if (sep == std::string::npos) return "";
    const std::string cls = qualified.substr(0, sep);
    const std::size_t clsSep = cls.rfind("::");
    std::string clsLast = clsSep == std::string::npos ? cls : cls.substr(clsSep + 2);
    clsLast = clsLast.substr(0, clsLast.find('<')); // "MemPoolT<120ul>" -> "MemPoolT"
    return qualified.substr(sep + 2) == clsLast ? cls : "";
}

struct Member {
    std::string owner;
    FieldId offset;
    std::string cls;
};

/// What the constructor calls in the graph say about classes.
struct Constructions {
    std::unordered_map<NodeId, std::string> objectClass;    ///< allocated object -> class
    std::vector<Member> members;                            ///< (owner, offset, class)
    std::vector<std::pair<EdgeId, std::string>> placements; ///< ctor call edge, class
};

Constructions classifyConstructions(const LDGraph& graph, const Hierarchy& hierarchy) {
    std::unordered_map<NodeId, std::vector<NodeId>> allocatedInto; // var -> objects
    for (const Edge& e : graph.edges())
        if (e.label == Label::New) allocatedInto[e.dst].push_back(e.src);

    // Pointers derived from a constructor's own `this`, with their offset.
    std::unordered_map<NodeId, FieldId> thisOffset;
    for (NodeId n = 0; n < graph.nodes().size(); ++n) {
        const Node& var = graph.nodes()[n];
        if (var.kind == NodeKind::Var && var.name == "this" &&
            !constructorClass(var.function).empty())
            thisOffset[n] = 0;
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (const Edge& e : graph.edges()) {
            if (e.dir != CallDir::None || (e.label != Label::Assign && e.label != Label::Gep))
                continue;
            auto from = thisOffset.find(e.src);
            if (from == thisOffset.end() || thisOffset.count(e.dst) != 0 ||
                graph.nodes()[e.dst].function != graph.nodes()[e.src].function)
                continue;
            if (e.label == Label::Gep && (e.field < 0 || from->second < 0)) continue;
            thisOffset[e.dst] = from->second + (e.label == Label::Gep ? e.field : 0);
            changed = true;
        }
    }

    Constructions result;
    for (EdgeId id = 0; id < graph.edges().size(); ++id) {
        const Edge& e = graph.edges()[id];
        const Node& formal = graph.nodes()[e.dst];
        if (e.dir != CallDir::Enter || formal.name != "this") continue;
        const std::string cls = constructorClass(formal.function);
        if (cls.empty()) continue;
        if (auto it = allocatedInto.find(e.src); it != allocatedInto.end()) {
            for (NodeId object : it->second) result.objectClass[object] = cls;
            continue;
        }
        if (auto it = thisOffset.find(e.src); it != thisOffset.end()) {
            const std::string owner = constructorClass(graph.nodes()[e.src].function);
            // Offset 0 is a base-class constructor, unless cls is not a base of owner
            // (then it is the first member).
            if (!owner.empty() && (it->second > 0 || !derives(hierarchy, owner, cls)))
                result.members.push_back({owner, it->second, cls});
            continue;
        }
        result.placements.push_back({id, cls});
    }
    return result;
}

// Virtual calls, paper Fig. 6 (PLAN.md §2.1): a_i --store[p_i] ⟦ĉ⟧--> r --assign--> r#c
// --dispatch[t] ĉ--> this --load[p_i]--> p_i; ret --store[ret]--> this; r --load[ret] ⟦č⟧--> x.

/// Nodes and edges to append to a graph; edges may refer to the new nodes by the ids they
/// will get.
struct Fragment {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
};

Fragment dispatchFragment(const LDGraph& graph) {
    Fragment fragment;
    std::unordered_set<NodeId> methodsDone; // by `this` formal
    for (CallSiteId c = 0; c < static_cast<CallSiteId>(graph.callSites().size()); ++c) {
        const CallSite& site = graph.callSites()[static_cast<std::size_t>(c)];
        if (!site.isVirtual || site.actuals.empty() || !site.actuals[0]) continue;
        const NodeId r = *site.actuals[0];
        const Node& receiver = graph.nodes()[r];
        const auto rc = static_cast<NodeId>(graph.nodes().size() + fragment.nodes.size());
        fragment.nodes.push_back(Node{NodeKind::RecvCopy, receiver.svfId,
                                      receiver.name + "#c" + std::to_string(c), receiver.function,
                                      site.line});
        auto add = [&](NodeId src, NodeId dst, Label label, FieldId field = kNoField,
                       CallSiteId callSite = kNoCallSite, CallDir dir = CallDir::None) {
            fragment.edges.push_back(Edge{src, dst, label, field, kUnknownType, callSite, dir});
        };
        add(r, rc, Label::Assign);
        for (std::size_t i = 1; i < site.actuals.size(); ++i)
            if (site.actuals[i])
                add(*site.actuals[i], r, Label::Store, paramField(static_cast<int>(i)), c,
                    CallDir::BoxEnter);
        if (site.actualRet) add(r, *site.actualRet, Label::Load, kRetField, c, CallDir::BoxExit);

        for (const VirtualTarget& target : site.targets) {
            if (target.formals.empty()) continue;
            const NodeId self = target.formals[0];
            for (TypeId t : target.types) {
                add(rc, self, Label::Dispatch, kNoField, c, CallDir::Enter);
                fragment.edges.back().type = t;
            }
            if (!methodsDone.insert(self).second) continue;
            for (std::size_t i = 1; i < target.formals.size(); ++i)
                add(self, target.formals[i], Label::Load, paramField(static_cast<int>(i)));
            if (target.ret) add(*target.ret, self, Label::Store, kRetField);
        }
    }
    return fragment;
}

/// Builds the graph from the values above. The only state: the graph itself, its SVF node
/// and call site maps, and the statistics.
class Builder {
public:
    Builder(SVFIR& pag, const CallGraph& callGraph, Mode mode, BuildStats& stats)
        : pag_(pag), callGraph_(callGraph), mode_(mode), stats_(stats) {}

    LDGraph run() {
        const Geps geps = collectGeps(pag_);
        const std::unordered_set<SvfId> escaping = findEscapingGeps(pag_, geps);
        stats_.variantGeps = geps.variant;

        add(statementEdges(pag_, geps, escaping));
        add(indirectCalls(pag_, callGraph_, mode_ == Mode::Lfc));

        const std::vector<VTable> vtables = collectVTables(pag_);
        const Hierarchy hierarchy = collectHierarchy();
        add(virtualSites(pag_, vtables, hierarchy));
        assignTypes(classifyConstructions(graph_, hierarchy), vtables, hierarchy);
        if (mode_ == Mode::Ldc || mode_ == Mode::Ldcr) add(dispatchFragment(graph_));
        return std::move(graph_);
    }

private:
    NodeId node(const SVFVar* var) {
        const bool isObj = SVFUtil::isa<ObjVar>(var);
        std::string name = var->getName();
        if (name.empty())
            name = (isObj ? "o" : "v") + std::to_string(var->getId());
        else if (isObj)
            name = "o" + std::to_string(var->getId()) + ":" + name;
        std::string function;
        if (!isObj)
            if (const FunObjVar* fun = var->getFunction())
                function = llvm::demangle(fun->getName());
        const NodeId id = graph_.nodeFor(var->getId(), isObj ? NodeKind::Obj : NodeKind::Var, name,
                                         function, parseLine(var->getSourceLoc()));
        // Field limit as SVF has it after Andersen: 0 = SVF made the object field-insensitive
        // (e.g. after pointer arithmetic over its fields); we follow, so all fields are `*`.
        if (const auto* object = SVFUtil::dyn_cast<BaseObjVar>(var))
            graph_.node(id).fieldLimit = static_cast<FieldId>(
                std::min<u32_t>(object->getMaxFieldOffsetLimit(), kDefaultFieldLimit));
        return id;
    }

    std::optional<NodeId> optionalNode(const SVFVar* var) {
        if (!relevant(var)) return std::nullopt;
        return node(var);
    }

    CallSiteId callSite(const CallICFGNode* cs) {
        auto it = callSiteIds_.find(cs);
        if (it != callSiteIds_.end()) return it->second;
        CallSite site;
        site.caller = llvm::demangle(cs->getCaller()->getName());
        site.line = parseLine(cs->getSourceLoc());
        site.isVirtual = cs->isVirtualCall();
        CallSiteId id = graph_.addCallSite(site);
        callSiteIds_.emplace(cs, id);
        return id;
    }

    void add(const SvfEdges& edges) {
        for (const SvfEdge& e : edges) {
            const CallSiteId site = e.site != nullptr ? callSite(e.site) : kNoCallSite;
            if (!relevant(e.src) || !relevant(e.dst)) {
                ++stats_.skippedStmts;
                continue;
            }
            const NodeId from = node(e.src); // before node(e.dst): node ids follow this order
            graph_.addEdge(Edge{from, node(e.dst), e.label, e.field, kUnknownType, site, e.dir});
        }
    }

    void add(const IndirectCalls& calls) {
        for (const CallICFGNode* cs : calls.sites) callSite(cs);
        stats_.indirectEdges += calls.edges.size();
        add(calls.edges);
    }

    void add(const std::vector<VirtualSite>& sites) {
        LLVMModuleSet* modules = LLVMModuleSet::getLLVMModuleSet();
        for (const VirtualSite& site : sites) {
            const CallSiteId id = callSite(site.cs);
            stats_.sitesWithoutDeclaredType += !site.declared;
            CallSite& info = graph_.callSite(id);
            for (const ValVar* actual : site.cs->getActualParms())
                info.actuals.push_back(optionalNode(actual));
            info.actualRet = optionalNode(site.cs->getRetICFGNode()->getActualRet());

            std::vector<const llvm::Function*> order;
            std::unordered_map<const llvm::Function*, std::vector<NodeId>> vtablesOf;
            for (const auto& [fn, vtable] : site.slots) {
                if (vtablesOf.find(fn) == vtablesOf.end()) order.push_back(fn);
                vtablesOf[fn].push_back(node(vtable));
            }
            for (const llvm::Function* fn : order) {
                const FunObjVar* callee = modules->getFunObjVar(fn);
                VirtualTarget target;
                target.callee = llvm::demangle(fn->getName().str());
                target.vtables = vtablesOf[fn];
                if (pag_.hasFunArgsList(callee))
                    for (const ValVar* formal : pag_.getFunArgsList(callee))
                        target.formals.push_back(node(formal));
                if (auto it = pag_.getFunRets().find(callee); it != pag_.getFunRets().end())
                    target.ret = optionalNode(it->second);
                info.targets.push_back(std::move(target));
            }
        }
    }

    void add(const Fragment& fragment) {
        for (const Node& n : fragment.nodes) graph_.addNode(n);
        for (const Edge& e : fragment.edges) graph_.addEdge(e);
    }

    /// Types objects and member subobjects, adds construction-site objects (placement new),
    /// and the dynamic types of each virtual call target.
    void assignTypes(Constructions constructions, const std::vector<VTable>& vtables,
                     const Hierarchy& hierarchy) {
        std::unordered_map<std::string, NodeId> vtableByClass;
        const std::string prefix = "vtable for ";
        for (const VTable& vtable : vtables) {
            const std::string name = llvm::demangle(vtable.global->getName().str());
            if (name.rfind(prefix, 0) == 0 && relevant(vtable.object))
                vtableByClass.emplace(name.substr(prefix.size()), node(vtable.object));
        }

        // Classes that matter for dispatch: polymorphic, or with such a member (transitively).
        std::set<std::string> relevantClasses;
        for (const auto& entry : vtableByClass) relevantClasses.insert(entry.first);
        for (bool changed = true; changed;) {
            changed = false;
            for (const Member& m : constructions.members)
                if (relevantClasses.count(m.cls) != 0 && relevantClasses.insert(m.owner).second)
                    changed = true;
        }

        std::unordered_map<NodeId, std::string>& objectClass = constructions.objectClass;
        for (const auto& [id, cls] : constructions.placements) {
            if (relevantClasses.count(cls) == 0) continue;
            const Edge call = graph_.edges()[id];
            const CallSite& site = graph_.callSites()[static_cast<std::size_t>(call.callSite)];
            const NodeId object = graph_.addNode(Node{
                NodeKind::Obj, kNoSvfId, cls + "@" + std::to_string(site.line), "", site.line});
            graph_.addEdge(Edge{object, call.src, Label::New});
            objectClass[object] = cls;
        }

        auto typeOf = [&](const std::string& cls) {
            auto vtable = vtableByClass.find(cls);
            return vtable == vtableByClass.end() ? kUnknownType
                                                 : graph_.typeFor(cls, vtable->second);
        };
        for (const auto& [object, cls] : objectClass) {
            graph_.node(object).type = typeOf(cls);
            // Member subobjects of cls and of its bases, nested.
            std::vector<std::pair<std::string, FieldId>> pending{{cls, 0}};
            for (std::size_t i = 0; i < pending.size() && i < 256; ++i) {
                const auto [owner, base] = pending[i];
                for (const Member& m : constructions.members) {
                    if (!derives(hierarchy, owner, m.owner)) continue;
                    const FieldId offset = base + m.offset;
                    if (TypeId type = typeOf(m.cls); type != kUnknownType)
                        graph_.setSubobjectType(object, offset, type);
                    pending.push_back({m.cls, offset});
                }
            }
        }

        for (CallSiteId c = 0; c < static_cast<CallSiteId>(graph_.callSites().size()); ++c)
            for (VirtualTarget& target : graph_.callSite(c).targets)
                for (TypeId t = 0; t < static_cast<TypeId>(graph_.types().size()); ++t)
                    for (NodeId vtable : target.vtables)
                        if (graph_.types()[static_cast<std::size_t>(t)].vtable == vtable)
                            target.types.push_back(t);
    }

    SVFIR& pag_;
    const CallGraph& callGraph_;
    Mode mode_;
    BuildStats& stats_;
    LDGraph graph_;
    std::unordered_map<const CallICFGNode*, CallSiteId> callSiteIds_;
};

} // namespace

LDGraph buildLDGraph(SVFIR& pag, const CallGraph& callGraph, Mode mode, BuildStats& stats) {
    return Builder(pag, callGraph, mode, stats).run();
}

} // namespace ldc
