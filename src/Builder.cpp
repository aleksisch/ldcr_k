#include "ldc/Builder.h"
#include "SVFDetails.h"
#include "ldc/SVFFrontend.h"
#include <stdexcept>

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

using frontend::detail::IndirectCalls;
using frontend::detail::relevant;
using frontend::detail::SvfEdge;
using frontend::detail::SvfEdges;

bool isIntrinsicName(const std::string& name) { return name.rfind("llvm.", 0) == 0; }

Label analysisLabel(frontend::Label label) {
    switch (label) {
    case frontend::Label::New: return Label::New;
    case frontend::Label::Assign: return Label::Assign;
    case frontend::Label::Store: return Label::Store;
    case frontend::Label::Load: return Label::Load;
    case frontend::Label::Gep: return Label::Gep;
    }
    throw std::logic_error("Unknown frontend edge label");
}
CallDir analysisDirection(frontend::CallDir direction) {
    switch (direction) {
    case frontend::CallDir::None: return CallDir::None;
    case frontend::CallDir::Enter: return CallDir::Enter;
    case frontend::CallDir::Exit: return CallDir::Exit;
    }
    throw std::logic_error("Unknown frontend call direction");
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
        const auto statements = frontend::detail::statementEdges(pag_);
        stats_.variantGeps = statements.variantGeps;
        add(statements.edges);
        add(frontend::detail::indirectCalls(pag_, callGraph_, mode_ == Mode::Lfc));

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
        const NodeId id =
            graph_.nodeFor(var->getId(), isObj ? NodeKind::Obj : NodeKind::Var, name, function,
                           frontend::detail::sourceLine(var->getSourceLoc()));
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
        site.line = frontend::detail::sourceLine(cs->getSourceLoc());
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
            graph_.addEdge(Edge{from, node(e.dst), analysisLabel(e.label),
                                (e.field ? e.field->value : kNoField), kUnknownType, site,
                                analysisDirection(e.dir)});
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
