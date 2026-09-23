#include "ldc/Builder.h"

#include "Graphs/CallGraph.h"
#include "Graphs/ICFGNode.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFStatements.h"
#include "SVFIR/SVFVariables.h"
#include "SVF-LLVM/LLVMModule.h"

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include <optional>
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
       << " skipped statements\n";
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
        assignTypes();
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
        return graph_.nodeFor(var->getId(), isObj ? NodeKind::Obj : NodeKind::Var, name,
                              function, parseLine(var->getSourceLoc()));
    }

    void edge(const SVFVar* src, const SVFVar* dst, Label label, FieldId field = kNoField,
              CallSiteId callSite = kNoCallSite, CallDir dir = CallDir::None)
    {
        if (!relevant(src) || !relevant(dst))
        {
            ++stats_.skippedStmts;
            return;
        }
        Edge e{node(src), node(dst), label};
        e.field = field;
        e.callSite = callSite;
        e.dir = dir;
        graph_.addEdge(e);
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
            FieldId field = kAnyField;
            if (gep->isConstantOffset())
                field = static_cast<FieldId>(gep->accumulateConstantOffset());
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

    /// A gep result used as a value (stored, copied, passed) — in C++ mostly the vptr
    /// `&vtable[2]` stored by constructors. LDGraph has no "pointer to field" values, so it
    /// gets the objects of its base (the field offset is dropped). This is what M4 reads
    /// to recover dynamic types: an object's field 0 holds its class's vtable.
    void addEscapingGeps()
    {
        for (SvfId id : stats_.escapingGepIds)
            edge(gepDef_.at(id).first, pag_.getGNode(id), Label::Assign);
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
        if (cs->isVirtualCall())
            describeVirtualTarget(site, cs, callee);
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
    }

    // ---- dynamic types -------------------------------------------------------
    //
    // As in paper [C-New], an object's type is fixed at its allocation. In C++ the
    // allocation `new T(...)` is `%p = operator new(...)` followed by the constructor
    // `T::T(%p)` called directly on %p; base-class constructors get `this` instead.
    // So the class of an object = the constructor whose `this` receives the allocated
    // pointer. (Reading the vptr from field 0 does not work flow-insensitively: base
    // constructors store their own vtable there first.)

    /// "ns::X::X(int)" -> "ns::X"; empty if `function` is not a constructor.
    static std::string constructorClass(const std::string& function)
    {
        const std::string qualified = function.substr(0, function.find('('));
        const std::size_t sep = qualified.rfind("::");
        if (sep == std::string::npos)
            return "";
        const std::string cls = qualified.substr(0, sep);
        const std::size_t clsSep = cls.rfind("::");
        const std::string clsLast = clsSep == std::string::npos ? cls : cls.substr(clsSep + 2);
        return qualified.substr(sep + 2) == clsLast ? cls : "";
    }

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

        for (EdgeId id = 0; id < graph_.edges().size(); ++id)
        {
            const Edge& e = graph_.edges()[id];
            const Node& formal = graph_.nodes()[e.dst];
            if (e.dir != CallDir::Enter || formal.name != "this")
                continue;
            const std::string cls = constructorClass(formal.function);
            auto vtable = vtableByClass.find(cls);
            if (cls.empty() || vtable == vtableByClass.end())
                continue;
            const TypeId type = graph_.typeFor(cls, vtable->second);
            for (NodeId object : allocatedInto[e.src])
                graph_.node(object).type = type;
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

    /// Records, for a virtual call site, its receiver/actuals and the target `callee` with
    /// the vtables that dispatch to it here. SVF's CHG (getVFnsFromVtbls) returns nothing
    /// in this SVF build, so the vtables are read directly (vtableSlot).
    void describeVirtualTarget(CallSiteId site, const CallICFGNode* cs, const FunObjVar* callee)
    {
        CallSite& info = graph_.callSite(site);
        if (info.actuals.empty())
        {
            for (const ValVar* actual : cs->getActualParms())
                info.actuals.push_back(optionalNode(actual));
            info.actualRet = optionalNode(cs->getRetICFGNode()->getActualRet());
        }

        VirtualTarget target;
        target.callee = demangle(callee->getName());
        for (const VTable& vtable : vtables())
        {
            const llvm::Function* fn = vtableSlot(vtable.global, cs->getFunIdxInVtable());
            if (fn != nullptr && fn->getName() == callee->getName() && relevant(vtable.object))
                target.vtables.push_back(node(vtable.object));
        }
        if (pag_.hasFunArgsList(callee))
            for (const ValVar* formal : pag_.getFunArgsList(callee))
                target.formals.push_back(node(formal));
        const auto& funRets = pag_.getFunRets();
        if (auto it = funRets.find(callee); it != funRets.end())
            target.ret = optionalNode(it->second);
        info.targets.push_back(std::move(target));
    }

    SVFIR& pag_;
    const CallGraph& callGraph_;
    std::vector<VTable> vtables_;
    bool vtablesCollected_ = false;
    BuildStats& stats_;
    LDGraph graph_;
    std::unordered_map<SvfId, std::pair<const SVFVar*, FieldId>> gepDef_;
    std::unordered_map<const CallICFGNode*, CallSiteId> callSiteIds_;
};

} // namespace

LDGraph buildLDGraph(SVFIR& pag, const CallGraph& callGraph, BuildStats& stats)
{
    return Builder(pag, callGraph, stats).run();
}

} // namespace ldc
