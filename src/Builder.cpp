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

namespace ldc
{

void BuildStats::print(std::ostream& os) const
{
    os << "Build: " << indirectEdges << " indirect call/return edges from Andersen, " << variantGeps
       << " variable-offset geps, " << escapingGeps << " escaping geps, " << skippedStmts
       << " skipped statements\n"
       << "Virtual calls: " << virtualSites << " sites, " << chaTargets << " CHA targets, "
       << andersenTargets << " Andersen targets, " << sitesWithoutDeclaredType
       << " sites without a declared type\n"
       << "Construction sites (placement new): " << constructionSites << "\n";
}

namespace
{

int parseLine(const std::string& sourceLoc)
{
    static const std::regex lineRe(R"re("?ln"?\s*:\s*(\d+))re");
    std::smatch m;
    if (std::regex_search(sourceLoc, m, lineRe))
        return std::stoi(m[1].str());
    return 0;
}

std::string demangle(const std::string& name)
{
    return llvm::demangle(name);
}

bool isIntrinsicName(const std::string& name)
{
    return name.rfind("llvm.", 0) == 0;
}

class Builder
{
public:
    Builder(SVFIR& pag, const CallGraph& callGraph, BuildStats& stats)
        : pag_(pag), callGraph_(callGraph), stats_(stats)
    {
    }

    LDGraph run()
    {
        collectGeps();
        findEscapingGeps();
        addEscapingGeps();
        addAddr();
        addCopies();
        addMultiOperand<PhiStmt>(SVFStmt::Phi);
        addMultiOperand<SelectStmt>(SVFStmt::Select);
        addStores();
        addLoads();
        addDirectCalls();
        addDirectReturns();
        addIndirectCalls();
        describeVirtualSites();
        assignTypes();
        addDispatch();
        return std::move(graph_);
    }

private:
    // ---- node filtering and creation ---------------------------------------

    /// Variables and objects that take part in pointer flow. Constants, dummies,
    /// field objects (created by SVF's analyses, not by the program) and values
    /// inside LLVM intrinsics are left out.
    bool relevant(const SVFVar* var) const
    {
        if (var == nullptr)
            return false;
        if (SVFUtil::isa<ConstDataValVar>(var) || SVFUtil::isa<ConstDataObjVar>(var) ||
            SVFUtil::isa<DummyValVar>(var) || SVFUtil::isa<DummyObjVar>(var) ||
            SVFUtil::isa<GepObjVar>(var))
            return false;
        if (const FunObjVar* fun = var->getFunction())
            if (isIntrinsicName(fun->getName()))
                return false;
        return true;
    }

    NodeId node(const SVFVar* var)
    {
        const bool isObj = SVFUtil::isa<ObjVar>(var);
        std::string name = var->getName();
        if (name.empty())
            name = (isObj ? "o" : "v") + std::to_string(var->getId());
        else if (isObj)
            name = "o" + std::to_string(var->getId()) + ":" + name;
        std::string function;
        if (!isObj)
            if (const FunObjVar* fun = var->getFunction())
                function = demangle(fun->getName());
        const NodeId id = graph_.nodeFor(var->getId(), isObj ? NodeKind::Obj : NodeKind::Var,
                                         name, function, parseLine(var->getSourceLoc()));
        // Field limit as SVF has it after Andersen: 0 = SVF made the object field-insensitive
        // (e.g. after pointer arithmetic over its fields); we follow, so all fields are `*`.
        if (const auto* object = SVFUtil::dyn_cast<BaseObjVar>(var))
            graph_.node(id).fieldLimit =
                static_cast<FieldId>(std::min<u32_t>(object->getMaxFieldOffsetLimit(), kDefaultFieldLimit));
        return id;
    }

    void edge(const SVFVar* src, const SVFVar* dst, Label label, FieldId field = kNoField,
              CallSiteId callSite = kNoCallSite, CallDir dir = CallDir::None)
    {
        if (!relevant(src) || !relevant(dst))
        {
            ++stats_.skippedStmts;
            return;
        }
        const NodeId from = node(src); // before node(dst): node ids follow this order
        addEdge(from, node(dst), label, field, callSite, dir);
    }

    CallSiteId callSite(const CallICFGNode* cs)
    {
        auto it = callSiteIds_.find(cs);
        if (it != callSiteIds_.end())
            return it->second;
        CallSite site;
        site.caller = demangle(cs->getCaller()->getName());
        site.line = parseLine(cs->getSourceLoc());
        site.isVirtual = cs->isVirtualCall();
        CallSiteId id = graph_.addCallSite(site);
        callSiteIds_.emplace(cs, id);
        return id;
    }

    // ---- fields -------------------------------------------------------------
    //
    // SVFIR models `p->f = v` as  q = gep p, f;  *q = v.  L_D wants  v --store[f]--> p.
    // So a gep produces no edge: its result is recorded here, and a load/store
    // whose address is a gep result is folded onto the gep's base with field f.
    // A direct dereference (*p) is field 0, which is also the first field of p.

    void collectGeps()
    {
        for (const SVFStmt* stmt : pag_.getSVFStmtSet(SVFStmt::Gep))
        {
            const auto* gep = SVFUtil::cast<GepStmt>(stmt);
            // As SVF's own field model: array indices (constant or not) are ignored, so an
            // element access lands on the array's first field; only pointer arithmetic over
            // struct fields (a "variant field" gep) loses the field.
            FieldId field = kAnyField;
            if (!gep->isVariantFieldGep())
                field = static_cast<FieldId>(gep->getConstantStructFldIdx());
            else
                ++stats_.variantGeps;
            gepDef_[gep->getLHSVarID()] = {gep->getRHSVar(), field};
        }
    }

    /// Resolves an address to (base pointer, field), following chains of geps.
    std::pair<const SVFVar*, FieldId> resolveAddress(const SVFVar* ptr) const
    {
        FieldId field = 0;
        const SVFVar* base = ptr;
        for (auto it = gepDef_.find(base->getId()); it != gepDef_.end();
             it = gepDef_.find(base->getId()))
        {
            const FieldId step = it->second.second;
            field = (field == kAnyField || step == kAnyField) ? kAnyField : field + step;
            base = it->second.first;
        }
        return {base, field};
    }

    /// A gep result used as a value (stored, copied, passed, returned, merged by phi/select): the vptr `&vtable[2]` stored by
    /// constructors, `&obj->member` passed as `this` of a member's method, ... It becomes
    /// `base --gep[f]--> q`: q points to the field objects ⟨O, off + f⟩ (like SVF's GepObjVar).
    /// Dropping the offset instead merges a member object with field 0 of its container.
    void addEscapingGeps()
    {
        for (SvfId id : stats_.escapingGepIds)
        {
            const SVFVar* gep = pag_.getGNode(id);
            const auto [base, field] = resolveAddress(gep);
            edge(base, gep, Label::Gep, field);
        }
    }

    void findEscapingGeps()
    {
        std::unordered_set<SvfId>& escaping = stats_.escapingGepIds;
        auto check = [&](const SVFVar* var) {
            if (var != nullptr && gepDef_.count(var->getId()) != 0)
                escaping.insert(var->getId());
        };
        for (const SVFStmt* stmt : pag_.getSVFStmtSet(SVFStmt::Copy))
            check(SVFUtil::cast<CopyStmt>(stmt)->getRHSVar());
        for (const SVFStmt* stmt : pag_.getSVFStmtSet(SVFStmt::Store))
            check(SVFUtil::cast<StoreStmt>(stmt)->getRHSVar()); // stored value, not the address
        for (const SVFStmt* stmt : pag_.getSVFStmtSet(SVFStmt::Call))
            for (const ValVar* op : SVFUtil::cast<CallPE>(stmt)->getOpndVars())
                check(op);
        for (const SVFStmt* stmt : pag_.getSVFStmtSet(SVFStmt::Ret))
            check(SVFUtil::cast<RetPE>(stmt)->getRHSVar()); // returned
        for (const auto& entry : pag_.getFunRets())
            check(entry.second); // returned (also from functions without callers)
        for (const SVFStmt* stmt : pag_.getSVFStmtSet(SVFStmt::Phi))
            for (const ValVar* op : SVFUtil::cast<PhiStmt>(stmt)->getOpndVars())
                check(op);
        for (const SVFStmt* stmt : pag_.getSVFStmtSet(SVFStmt::Select))
            for (const ValVar* op : SVFUtil::cast<SelectStmt>(stmt)->getOpndVars())
                check(op);
        stats_.escapingGeps = escaping.size();
    }

    // ---- statements ---------------------------------------------------------

    void addAddr()
    {
        for (const SVFStmt* stmt : pag_.getSVFStmtSet(SVFStmt::Addr))
        {
            const auto* addr = SVFUtil::cast<AddrStmt>(stmt);
            edge(addr->getRHSVar(), addr->getLHSVar(), Label::New);
        }
    }

    void addCopies()
    {
        for (const SVFStmt* stmt : pag_.getSVFStmtSet(SVFStmt::Copy))
        {
            const auto* copy = SVFUtil::cast<CopyStmt>(stmt);
            edge(copy->getRHSVar(), copy->getLHSVar(), Label::Assign);
        }
    }

    template <typename Stmt> void addMultiOperand(SVFStmt::PEDGEK kind)
    {
        for (const SVFStmt* stmt : pag_.getSVFStmtSet(kind))
        {
            const auto* multi = SVFUtil::cast<Stmt>(stmt);
            for (const ValVar* op : multi->getOpndVars())
                edge(op, multi->getRes(), Label::Assign);
        }
    }

    void addStores()
    {
        // StoreStmt: *LHS = RHS  →  RHS --store[f]--> base(LHS)
        for (const SVFStmt* stmt : pag_.getSVFStmtSet(SVFStmt::Store))
        {
            const auto* store = SVFUtil::cast<StoreStmt>(stmt);
            auto [base, field] = resolveAddress(store->getLHSVar());
            edge(store->getRHSVar(), base, Label::Store, field);
        }
    }

    void addLoads()
    {
        // LoadStmt: LHS = *RHS  →  base(RHS) --load[f]--> LHS
        for (const SVFStmt* stmt : pag_.getSVFStmtSet(SVFStmt::Load))
        {
            const auto* load = SVFUtil::cast<LoadStmt>(stmt);
            auto [base, field] = resolveAddress(load->getRHSVar());
            edge(base, load->getLHSVar(), Label::Load, field);
        }
    }

    void addDirectCalls()
    {
        // CallPE: formal param (res) ← one actual per call site (operand i at site i).
        for (const SVFStmt* stmt : pag_.getSVFStmtSet(SVFStmt::Call))
        {
            const auto* call = SVFUtil::cast<CallPE>(stmt);
            for (u32_t i = 0; i < call->getOpVarNum(); ++i)
                edge(call->getOpVar(i), call->getRes(), Label::Assign, kNoField,
                     callSite(call->getOpCallICFGNode(i)), CallDir::Enter);
        }
    }

    void addDirectReturns()
    {
        // RetPE: formal return (RHS) → actual return at the call site (LHS).
        for (const SVFStmt* stmt : pag_.getSVFStmtSet(SVFStmt::Ret))
        {
            const auto* ret = SVFUtil::cast<RetPE>(stmt);
            edge(ret->getRHSVar(), ret->getLHSVar(), Label::Assign, kNoField,
                 callSite(ret->getCallInst()), CallDir::Exit);
        }
    }

    /// SVFIR has no CallPE/RetPE for indirect (incl. virtual) call sites: SVF's
    /// Andersen connects them only in its own constraint graph. Wire them here
    /// from the resolved call graph — the L_FC way ([P-VCall]).
    void addIndirectCalls()
    {
        for (const auto& entry : callGraph_)
        {
            for (const CallGraphEdge* cgEdge : entry.second->getOutEdges())
            {
                if (!cgEdge->isIndirectCallEdge())
                    continue;
                const FunObjVar* callee = cgEdge->getDstNode()->getFunction();
                for (const CallICFGNode* cs : cgEdge->getIndirectCalls())
                    wireIndirect(cs, callee);
            }
        }
    }

    void wireIndirect(const CallICFGNode* cs, const FunObjVar* callee)
    {
        const CallSiteId site = callSite(cs);
        encoding_ = cs->isVirtualCall() ? Encoding::Fc : Encoding::Common;
        stats_.andersenTargets += cs->isVirtualCall();
        if (pag_.hasFunArgsList(callee))
        {
            const auto& formals = pag_.getFunArgsList(callee);
            const auto& actuals = cs->getActualParms();
            for (std::size_t i = 0; i < formals.size() && i < actuals.size(); ++i)
            {
                edge(actuals[i], formals[i], Label::Assign, kNoField, site, CallDir::Enter);
                ++stats_.indirectEdges;
            }
        }
        const auto& funRets = pag_.getFunRets();
        auto retIt = funRets.find(callee);
        const SVFVar* actualRet = cs->getRetICFGNode()->getActualRet();
        if (retIt != funRets.end() && actualRet != nullptr)
        {
            edge(retIt->second, actualRet, Label::Assign, kNoField, site, CallDir::Exit);
            ++stats_.indirectEdges;
        }
        encoding_ = Encoding::Common;
    }

    // ---- virtual calls, paper Fig. 6 ([C-VCall], [C-Param], [C-Ret]) ---------
    //
    //   a_i  --store[p_i] ⟦ĉ⟧-->  r            (caller: argument into the receiver)
    //   r    --assign-->          r#c
    //   r#c  --dispatch[t] ĉ-->   this^m'      for each t with m' = dispatch(c, t)  (CHA)
    //   this^m' --load[p_i]-->    p_i^m'       (callee, once per method)
    //   ret^m'  --store[ret]-->   this^m'      (callee, once per method)
    //   r    --load[ret] ⟦č⟧-->   x            (caller: the call's result)
    //
    // The paper also has a boxed r --assign ⟦č⟧--> r#c for the argument excursion; for
    // L_D ∩ C_k it is the same edge (boxed labels are ε there). Phase 2 (L_R) adds it.

    void addDispatch()
    {
        encoding_ = Encoding::D;
        std::unordered_set<NodeId> methodsDone; // by `this` formal
        for (CallSiteId c = 0; c < static_cast<CallSiteId>(graph_.callSites().size()); ++c)
        {
            const CallSite site = graph_.callSites()[static_cast<std::size_t>(c)];
            if (!site.isVirtual || site.actuals.empty() || !site.actuals[0])
                continue;
            const NodeId r = *site.actuals[0];
            const Node receiver = graph_.nodes()[r];
            const NodeId rc = graph_.addNode(Node{NodeKind::RecvCopy, receiver.svfId,
                                                  receiver.name + "#c" + std::to_string(c),
                                                  receiver.function, site.line});
            addEdge(r, rc, Label::Assign);
            for (std::size_t i = 1; i < site.actuals.size(); ++i)
                if (site.actuals[i])
                    addEdge(*site.actuals[i], r, Label::Store, paramField(static_cast<int>(i)), c,
                            CallDir::BoxEnter);
            if (site.actualRet)
                addEdge(r, *site.actualRet, Label::Load, kRetField, c, CallDir::BoxExit);

            for (const VirtualTarget& target : site.targets)
            {
                if (target.formals.empty())
                    continue;
                const NodeId self = target.formals[0];
                for (TypeId t : target.types)
                {
                    Edge e{rc, self, Label::Dispatch};
                    e.type = t;
                    e.callSite = c;
                    e.dir = CallDir::Enter;
                    e.encoding = Encoding::D;
                    graph_.addEdge(e);
                }
                if (!methodsDone.insert(self).second)
                    continue;
                for (std::size_t i = 1; i < target.formals.size(); ++i)
                    addEdge(self, target.formals[i], Label::Load, paramField(static_cast<int>(i)));
                if (target.ret)
                    addEdge(*target.ret, self, Label::Store, kRetField);
            }
        }
        encoding_ = Encoding::Common;
    }

    void addEdge(NodeId src, NodeId dst, Label label, FieldId field = kNoField,
                 CallSiteId callSite = kNoCallSite, CallDir dir = CallDir::None)
    {
        Edge e{src, dst, label};
        e.field = field;
        e.callSite = callSite;
        e.dir = dir;
        e.encoding = encoding_;
        graph_.addEdge(e);
    }

    // ---- dynamic types -------------------------------------------------------
    //
    // As in paper [C-New], an object's type is fixed at its allocation. In C++ the
    // allocation `new T(...)` is `%p = operator new(...)` followed by the constructor
    // `T::T(%p)` called directly on %p; base-class constructors get `this` instead.
    // So the class of an object = the constructor whose `this` receives the allocated
    // pointer. (Reading the vptr from field 0 does not work flow-insensitively: base
    // constructors store their own vtable there first.)

    // ---- class hierarchy (for DeclTypeOf filtering), from debug info -----------

    static std::string stripPrefix(const std::string& text, const std::string& prefix)
    {
        return text.rfind(prefix, 0) == 0 ? text.substr(prefix.size()) : "";
    }

    /// DeclTypeOf(r) of a virtual call. SVF's getFunNameOfVirtualCall() is empty here: it
    /// comes from "VCallFunName" metadata that only SVF's preprocessing adds (and that is also
    /// why SVF's CHG finds no targets). Instead, the IR is compiled with
    /// -fwhole-program-vtables: clang then tests the loaded vtable against the static class,
    /// `llvm.public.type.test(%vtable, !"_ZTS<class>")`.
    static std::string declaredClass(const CallICFGNode* cs)
    {
        const llvm::Value* vtable = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(cs->getVtablePtr());
        if (vtable == nullptr)
            return "";
        for (const llvm::User* user : vtable->users())
        {
            const auto* call = llvm::dyn_cast<llvm::CallBase>(user);
            const llvm::Function* callee = call ? call->getCalledFunction() : nullptr;
            if (callee == nullptr || (callee->getName() != "llvm.public.type.test" &&
                                      callee->getName() != "llvm.type.test"))
                continue;
            if (const auto* md = llvm::dyn_cast<llvm::MetadataAsValue>(call->getArgOperand(1)))
                if (const auto* id = llvm::dyn_cast<llvm::MDString>(md->getMetadata()))
                    return stripPrefix(demangle(id->getString().str()), "typeinfo name for ");
        }
        return "";
    }

    static std::string vtableClass(const llvm::GlobalVariable* vtable)
    {
        return stripPrefix(demangle(vtable->getName().str()), "vtable for ");
    }

    /// Class name of a C++ debug-info type, from its ODR identifier (_ZTS...).
    static std::string debugClass(const llvm::DICompositeType* type)
    {
        if (type == nullptr || type->getIdentifier().empty())
            return "";
        return stripPrefix(demangle(type->getIdentifier().str()), "typeinfo name for ");
    }

    void collectHierarchy()
    {
        LLVMModuleSet* modules = LLVMModuleSet::getLLVMModuleSet();
        for (u32_t i = 0; i < modules->getModuleNum(); ++i)
        {
            llvm::DebugInfoFinder finder;
            finder.processModule(*modules->getModule(i));
            for (const llvm::DIType* type : finder.types())
            {
                const auto* composite = llvm::dyn_cast<llvm::DICompositeType>(type);
                const std::string cls = debugClass(composite);
                if (cls.empty())
                    continue;
                auto& bases = bases_[cls];
                for (const llvm::DINode* element : composite->getElements())
                {
                    const auto* derived = llvm::dyn_cast<llvm::DIDerivedType>(element);
                    if (derived == nullptr || derived->getTag() != llvm::dwarf::DW_TAG_inheritance)
                        continue;
                    const std::string base =
                        debugClass(llvm::dyn_cast_or_null<llvm::DICompositeType>(derived->getBaseType()));
                    if (!base.empty())
                        bases.push_back(base);
                }
            }
        }
    }

    bool derives(const std::string& cls, const std::string& base) const
    {
        if (cls == base)
            return true;
        auto it = bases_.find(cls);
        if (it == bases_.end())
            return false;
        for (const std::string& direct : it->second)
            if (derives(direct, base))
                return true;
        return false;
    }

    /// "ns::X::X(int)" -> "ns::X", "ns::T<1>::T()" -> "ns::T<1>"; empty if `function` is not a
    /// constructor.
    static std::string constructorClass(const std::string& function)
    {
        const std::string qualified = function.substr(0, function.find('('));
        const std::size_t sep = qualified.rfind("::");
        if (sep == std::string::npos)
            return "";
        const std::string cls = qualified.substr(0, sep);
        const std::size_t clsSep = cls.rfind("::");
        std::string clsLast = clsSep == std::string::npos ? cls : cls.substr(clsSep + 2);
        clsLast = clsLast.substr(0, clsLast.find('<')); // "MemPoolT<120ul>" -> "MemPoolT"
        return qualified.substr(sep + 2) == clsLast ? cls : "";
    }

    // Beyond `new T(...)`, C++ constructs objects in memory that is not a fresh allocation:
    //  - placement new, `new (pool.Alloc()) T(...)`: the memory is some pool block. Such a
    //    *construction site* gets its own object of class T (`O --new[T]--> p`), next to the
    //    pool memory p already points to;
    //  - member subobjects, `T member;` in class C: C's constructor calls T::T(this + off).
    //    Recorded as (C, off, T); every object of class C (or a subclass) then has a T at off,
    //    also nested. An interior pointer ⟨O, off⟩ dispatches by that type.
    //  - base classes: B::B(this) from C's constructor, offset 0 — the same object.
    // Only classes that matter for dispatch get construction-site objects: polymorphic ones
    // and those with polymorphic members.

    struct Member
    {
        std::string owner;
        FieldId offset;
        std::string cls;
    };

    void assignTypes()
    {
        std::unordered_map<std::string, NodeId> vtableByClass;
        const std::string prefix = "vtable for ";
        for (const VTable& vtable : vtables())
        {
            const std::string name = demangle(vtable.global->getName().str());
            if (name.rfind(prefix, 0) == 0 && relevant(vtable.object))
                vtableByClass.emplace(name.substr(prefix.size()), node(vtable.object));
        }

        std::unordered_map<NodeId, std::vector<NodeId>> allocatedInto; // var -> objects
        for (const Edge& e : graph_.edges())
            if (e.label == Label::New)
                allocatedInto[e.dst].push_back(e.src);

        // Pointers derived from a constructor's own `this`, with their offset.
        std::unordered_map<NodeId, FieldId> thisOffset;
        for (NodeId n = 0; n < graph_.nodes().size(); ++n)
        {
            const Node& var = graph_.nodes()[n];
            if (var.kind == NodeKind::Var && var.name == "this" && !constructorClass(var.function).empty())
                thisOffset[n] = 0;
        }
        for (bool changed = true; changed;)
        {
            changed = false;
            for (const Edge& e : graph_.edges())
            {
                if (e.dir != CallDir::None || e.encoding != Encoding::Common ||
                    (e.label != Label::Assign && e.label != Label::Gep))
                    continue;
                auto from = thisOffset.find(e.src);
                if (from == thisOffset.end() || thisOffset.count(e.dst) != 0 ||
                    graph_.nodes()[e.dst].function != graph_.nodes()[e.src].function)
                    continue;
                if (e.label == Label::Gep && (e.field < 0 || from->second < 0))
                    continue;
                thisOffset[e.dst] = from->second + (e.label == Label::Gep ? e.field : 0);
                changed = true;
            }
        }

        std::unordered_map<NodeId, std::string> objectClass;
        std::vector<Member> members;
        std::vector<std::pair<EdgeId, std::string>> placements; // ctor call edge, class
        for (EdgeId id = 0; id < graph_.edges().size(); ++id)
        {
            const Edge& e = graph_.edges()[id];
            const Node& formal = graph_.nodes()[e.dst];
            if (e.dir != CallDir::Enter || formal.name != "this")
                continue;
            const std::string cls = constructorClass(formal.function);
            if (cls.empty())
                continue;
            if (auto it = allocatedInto.find(e.src); it != allocatedInto.end())
            {
                for (NodeId object : it->second)
                    objectClass[object] = cls;
                continue;
            }
            if (auto it = thisOffset.find(e.src); it != thisOffset.end())
            {
                const std::string owner = constructorClass(graph_.nodes()[e.src].function);
                // Offset 0 is a base-class constructor, unless cls is not a base of owner
                // (then it is the first member).
                if (!owner.empty() && (it->second > 0 || !derives(owner, cls)))
                    members.push_back({owner, it->second, cls});
                continue;
            }
            placements.push_back({id, cls});
        }

        // Classes that matter for dispatch: polymorphic, or with such a member (transitively).
        std::set<std::string> relevantClasses;
        for (const auto& entry : vtableByClass)
            relevantClasses.insert(entry.first);
        for (bool changed = true; changed;)
        {
            changed = false;
            for (const Member& m : members)
                if (relevantClasses.count(m.cls) != 0 && relevantClasses.insert(m.owner).second)
                    changed = true;
        }

        for (const auto& [id, cls] : placements)
        {
            if (relevantClasses.count(cls) == 0)
                continue;
            const Edge call = graph_.edges()[id];
            const CallSite& site = graph_.callSites()[static_cast<std::size_t>(call.callSite)];
            const NodeId object = graph_.addNode(Node{NodeKind::Obj, kNoSvfId,
                                                      cls + "@" + std::to_string(site.line), "",
                                                      site.line});
            addEdge(object, call.src, Label::New);
            objectClass[object] = cls;
            ++stats_.constructionSites;
        }

        auto typeOf = [&](const std::string& cls) {
            auto vtable = vtableByClass.find(cls);
            return vtable == vtableByClass.end() ? kUnknownType : graph_.typeFor(cls, vtable->second);
        };
        for (const auto& [object, cls] : objectClass)
        {
            graph_.node(object).type = typeOf(cls);
            // Member subobjects of cls and of its bases, nested.
            std::vector<std::pair<std::string, FieldId>> pending{{cls, 0}};
            for (std::size_t i = 0; i < pending.size() && i < 256; ++i)
            {
                const auto [owner, base] = pending[i];
                for (const Member& m : members)
                {
                    if (!derives(owner, m.owner))
                        continue;
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

    std::optional<NodeId> optionalNode(const SVFVar* var)
    {
        if (!relevant(var))
            return std::nullopt;
        return node(var);
    }

    /// A vtable of the program: its LLVM global and its SVF object node.
    struct VTable
    {
        const llvm::GlobalVariable* global;
        const SVFVar* object;
    };

    /// All vtables with an initializer (Itanium ABI: globals named _ZTV...). Taken from LLVM,
    /// then mapped to SVF objects: SVF names these objects after the class ("A"), not "_ZTV1A".
    const std::vector<VTable>& vtables()
    {
        if (!vtablesCollected_)
        {
            LLVMModuleSet* modules = LLVMModuleSet::getLLVMModuleSet();
            for (u32_t i = 0; i < modules->getModuleNum(); ++i)
                for (const llvm::GlobalVariable& global : modules->getModule(i)->globals())
                    if (global.getName().starts_with("_ZTV") && global.hasInitializer())
                        vtables_.push_back({&global, pag_.getGNode(modules->getObjectNode(&global))});
            vtablesCollected_ = true;
        }
        return vtables_;
    }

    /// The function in `slot` of a vtable, read from its LLVM initializer.
    /// Itanium ABI, single inheritance: `{ [N x ptr] [offset-to-top, RTTI, f0, f1, ...] }`
    /// and the vptr stored by constructors points at element 2 (the address point).
    /// Multiple inheritance (secondary vtables, thunks) is out of scope (PLAN.md §5).
    static const llvm::Function* vtableSlot(const llvm::GlobalVariable* vtable, s32_t slot)
    {
        constexpr unsigned kAddressPoint = 2;
        if (slot < 0)
            return nullptr;
        const llvm::Constant* array = vtable->getInitializer()->getAggregateElement(0u);
        if (array == nullptr)
            return nullptr;
        const llvm::Constant* entry = array->getAggregateElement(kAddressPoint + static_cast<unsigned>(slot));
        return entry ? llvm::dyn_cast<llvm::Function>(entry->stripPointerCasts()) : nullptr;
    }

    /// Records every virtual call site with its receiver/actuals and its CHA targets: the
    /// methods in the call's vtable slot, over all vtables of the program. SVF's CHG
    /// (getVFnsFromVtbls) returns nothing in this SVF build, so the vtables are read directly
    /// (vtableSlot). No declared-type filter: L_D's dispatch[t] filters by the receiver's type.
    void describeVirtualSites()
    {
        LLVMModuleSet* modules = LLVMModuleSet::getLLVMModuleSet();
        collectHierarchy();
        for (const CallICFGNode* cs : pag_.getCallSiteSet())
        {
            if (!cs->isVirtualCall() || isIntrinsicName(cs->getCaller()->getName()))
                continue;
            const CallSiteId site = callSite(cs);
            ++stats_.virtualSites;
            CallSite& info = graph_.callSite(site);
            for (const ValVar* actual : cs->getActualParms())
                info.actuals.push_back(optionalNode(actual));
            info.actualRet = optionalNode(cs->getRetICFGNode()->getActualRet());

            // [C-VCall]: only types t <: DeclTypeOf(r). The declared class is the class of the
            // method named at the call (unknown hierarchy: no filter).
            const std::string declared = declaredClass(cs);
            if (declared.empty())
                ++stats_.sitesWithoutDeclaredType;
            std::vector<const llvm::Function*> order;
            std::unordered_map<const llvm::Function*, std::vector<NodeId>> vtablesOf;
            for (const VTable& vtable : vtables())
            {
                if (!declared.empty() && bases_.count(declared) != 0 &&
                    !derives(vtableClass(vtable.global), declared))
                    continue;
                const llvm::Function* fn = vtableSlot(vtable.global, cs->getFunIdxInVtable());
                if (fn == nullptr || fn->isDeclaration() || !relevant(vtable.object))
                    continue;
                if (vtablesOf.find(fn) == vtablesOf.end())
                    order.push_back(fn);
                vtablesOf[fn].push_back(node(vtable.object));
            }

            for (const llvm::Function* fn : order)
            {
                const FunObjVar* callee = modules->getFunObjVar(fn);
                VirtualTarget target;
                target.callee = demangle(fn->getName().str());
                target.vtables = vtablesOf[fn];
                if (pag_.hasFunArgsList(callee))
                    for (const ValVar* formal : pag_.getFunArgsList(callee))
                        target.formals.push_back(node(formal));
                const auto& funRets = pag_.getFunRets();
                if (auto it = funRets.find(callee); it != funRets.end())
                    target.ret = optionalNode(it->second);
                info.targets.push_back(std::move(target));
                ++stats_.chaTargets;
            }
        }
    }

    SVFIR& pag_;
    const CallGraph& callGraph_;
    std::vector<VTable> vtables_;
    std::unordered_map<std::string, std::vector<std::string>> bases_; ///< class -> direct bases
    bool vtablesCollected_ = false;
    BuildStats& stats_;
    LDGraph graph_;
    Encoding encoding_ = Encoding::Common; ///< encoding of the edges being added
    std::unordered_map<SvfId, std::pair<const SVFVar*, FieldId>> gepDef_;
    std::unordered_map<const CallICFGNode*, CallSiteId> callSiteIds_;
};

} // namespace

LDGraph buildLDGraph(SVFIR& pag, const CallGraph& callGraph, BuildStats& stats)
{
    return Builder(pag, callGraph, stats).run();
}

} // namespace ldc
