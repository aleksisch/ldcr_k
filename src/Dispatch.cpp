#include "Dispatch.h"
#include "SVFDetails.h"
#include "Graphs/CHG.h"
#include "Graphs/ICFG.h"
#include "SVFIR/SVFIR.h"
#include "SVF-LLVM/CppUtil.h"
#include "SVF-LLVM/LLVMModule.h"
#include <llvm/Demangle/Demangle.h>
#include <algorithm>
#include <map>
#include <set>

using namespace SVF;

namespace ldc::frontend::detail {
namespace {

// Synthetic receiver fields carrying a virtual call's result and arguments.
constexpr FieldOffset kReturnField{-10};
constexpr FieldOffset parameterField(std::size_t index) {
    return FieldOffset{kReturnField.value - static_cast<std::int32_t>(index)};
}

class DispatchBuilder {
public:
    DispatchBuilder(SVFIR& pag, SimplifiedPAG& graph) : pag_(pag), graph_(graph) {}

    void run() {
        // Virtual calls pass arguments through the receiver instead of ordinary call/return edges.
        std::erase_if(graph_.edges(), [&](const auto& edge) {
            return edge.callSite && graph_.callSites()[edge.callSite->value].flags.isVirtual;
        });
        typeAllocations();
        for (auto i = 0u; i < graph_.callSites().size(); ++i) {
            if (graph_.callSites()[i].flags.isVirtual) {
                encode(CallSiteId{static_cast<std::int32_t>(i)});
            }
        }
    }

private:
    NodeId node(const SVFVar* var) {
        if (auto id = graph_.findSvf(SvfId{var->getId()})) return *id;
        Node info;
        info.svfId = SvfId{var->getId()};
        info.kind = SVFUtil::isa<ObjVar>(var) ? NodeKind::Obj : NodeKind::Var;
        info.name = var->getName();
        if (const auto* fn = var->getFunction()) info.function = llvm::demangle(fn->getName());
        info.sourceLocation = var->getSourceLoc();
        info.line = detail::sourceLine(info.sourceLocation);
        return graph_.addNode(info);
    }

    std::optional<NodeId> optionalNode(const SVFVar* var) {
        return detail::relevant(var) ? std::optional<NodeId>{node(var)} : std::nullopt;
    }

    // new[T]: an object passed from its allocation directly to T's constructor has class T.
    // Base-class constructors receive a `this` parameter, not an allocation, so they add nothing.
    void typeAllocations() {
        const auto* hierarchy = SVFUtil::dyn_cast<CHGraph>(pag_.getCHG());
        if (!hierarchy) return;
        auto* modules = LLVMModuleSet::getLLVMModuleSet();
        std::map<std::uint32_t, NodeId> constructed; // `this` actual -> class vtable
        for (const auto* site : pag_.getCallSiteSet()) {
            const auto* callee = site->getCalledFunction();
            if (!callee || site->getActualParms().empty() || !modules->hasLLVMValue(callee)) {
                continue;
            }
            const auto* fn = llvm::dyn_cast<llvm::Function>(modules->getLLVMValue(callee));
            if (!fn || !cppUtil::isConstructor(fn)) continue;
            const auto* cls = hierarchy->getNode(cppUtil::demangle(fn->getName().str()).className);
            const auto self = graph_.findSvf(SvfId{site->getActualParms()[0]->getId()});
            if (cls && detail::relevant(cls->getVTable()) && self) {
                constructed.emplace(self->value, node(cls->getVTable()));
            }
        }
        for (auto& edge : graph_.edges()) {
            if (edge.label != Label::New) continue;
            if (const auto it = constructed.find(edge.dst.value); it != constructed.end()) {
                edge.type = it->second;
            }
        }
    }

    // Methods the virtual call may run, each with the classes (by vtable) selecting it.
    // E.g. s->reset(): {Shape::reset -> (fn, [Square]), Circle::reset -> (fn, [Circle])}.
    auto chaTargets(const CallSite& info) {
        std::map<std::string, std::pair<const FunObjVar*, std::vector<NodeId>>> targets;
        if (!info.svfId) return targets;
        const auto* site =
            SVFUtil::cast<CallICFGNode>(pag_.getICFG()->getICFGNode(info.svfId->value));
        auto* cha = pag_.getCHG();
        if (!cha->csHasVtblsBasedonCHA(site)) return targets;
        for (const auto* vtable : cha->getCSVtblsBasedonCHA(site)) {
            if (!detail::relevant(vtable)) continue;
            const auto cls = node(vtable);
            VFunSet functions;
            cha->getVFnsFromVtbls(site, VTableSet{vtable}, functions);
            for (const auto* fn : functions) {
                auto& [function, vtables] = targets[fn->getName()];
                function = fn;
                if (std::find(vtables.begin(), vtables.end(), cls) == vtables.end()) {
                    vtables.push_back(cls);
                }
            }
        }
        return targets;
    }

    CallTarget callTarget(const std::string& name, const FunObjVar* function) {
        CallTarget target;
        target.function = llvm::demangle(name);
        if (pag_.hasFunArgsList(function)) {
            for (const auto* formal : pag_.getFunArgsList(function)) {
                target.formals.push_back(optionalNode(formal));
            }
        }
        if (auto ret = pag_.getFunRets().find(function); ret != pag_.getFunRets().end()) {
            target.ret = optionalNode(ret->second);
        }
        return target;
    }

    // Adds r#c and the caller side: r --assign--> r#c, a_i --store[p_i]--> r, r --load[ret]--> x.
    std::optional<NodeId> receiverCopy(CallSiteId id) {
        const auto& site = graph_.callSites()[id.value];
        if (site.actuals.empty() || !site.actuals[0]) return std::nullopt;
        const auto receiver = *site.actuals[0];
        auto copy = graph_.nodes()[receiver.value];
        copy.kind = NodeKind::Receiver;
        copy.svfId.reset();
        const auto suffix = "#c" + std::to_string(id.value);
        copy.name += suffix;
        if (!copy.sourceName.empty()) copy.sourceName += suffix;
        const auto rc = graph_.addNode(copy);
        graph_.addEdge({receiver, rc, Label::Assign});
        // Store arguments as receiver fields: a_i --store[p_i]--> r.
        for (auto p = std::size_t{1}; p < site.actuals.size(); ++p) {
            if (site.actuals[p]) {
                graph_.addEdge({*site.actuals[p], receiver, Label::Store, parameterField(p), id,
                                CallDir::DispatchEnter});
            }
        }
        // Load the return value from a receiver field: r --load[ret]--> x.
        if (site.actualRet) {
            graph_.addEdge({receiver, *site.actualRet, Label::Load, kReturnField, id,
                            CallDir::DispatchExit});
        }
        return rc;
    }

    // Replaces the call's targets with SVF's CHA candidates and adds their dispatch edges.
    void encode(CallSiteId id) {
        const auto rc = receiverCopy(id);
        auto targets = chaTargets(graph_.callSites()[id.value]);
        auto& site = graph_.callSite(id);
        site.targets.clear();
        for (const auto& [name, entry] : targets) {
            const auto& [function, vtables] = entry;
            auto target = callTarget(name, function);
            if (rc && !target.formals.empty() && target.formals[0]) {
                const auto self = *target.formals[0];
                for (auto vtable : vtables) {
                    graph_.addEdge(
                        {*rc, self, Label::Dispatch, std::nullopt, id, CallDir::Enter, vtable});
                }
                // Callee side is shared by all calls of the method.
                if (encodedMethods_.insert(self.value).second) {
                    for (auto p = std::size_t{1}; p < target.formals.size(); ++p) {
                        if (target.formals[p]) {
                            graph_.addEdge(
                                {self, *target.formals[p], Label::Load, parameterField(p)});
                        }
                    }
                    if (target.ret) graph_.addEdge({*target.ret, self, Label::Store, kReturnField});
                }
            }
            site.targets.push_back(std::move(target));
        }
    }

    SVFIR& pag_;
    SimplifiedPAG& graph_;
    std::set<std::uint32_t> encodedMethods_;
};

}

void buildDispatchGraph(SVFIR& pag, SimplifiedPAG& graph) {
    DispatchBuilder(pag, graph).run();
}

}
