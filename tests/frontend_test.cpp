#include "ldc/SimplifiedPAG.h"
#include "ldc/SVFFrontend.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "WPA/Andersen.h"
#include <iostream>
#include <limits>
#include <type_traits>
#include <sstream>
#include <stdexcept>

using namespace ldc::frontend;

static_assert(!std::is_convertible_v<SvfId, NodeId>);
static_assert(!std::is_convertible_v<NodeId, CallSiteId>);
static_assert(!std::is_convertible_v<FieldOffset, NodeId>);
static_assert(!std::is_convertible_v<std::uint32_t, NodeId>);
static_assert(!std::is_convertible_v<NodeId, std::uint32_t>);

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool hasEdge(const SimplifiedPAG& graph, NodeId from, NodeId to, CallDir dir, CallSiteId site) {
    for (const auto& edge : graph.edges())
        if (edge.src == from && edge.dst == to && edge.label == Label::Assign && edge.dir == dir &&
            edge.callSite == site)
            return true;
    return false;
}

void check(const SimplifiedPAG& graph, bool debug) {
    bool allocation = false, store = false, load = false, gep = false, merge = false;
    bool sourceName = false, location = false, demangled = false;
    for (std::uint32_t id = 0; id < graph.nodes().size(); ++id) {
        const auto& node = graph.nodes()[id];
        require(node.svfId && graph.findSvf(*node.svfId) == NodeId{id}, "SVF ID mapping lost");
        require(!node.name.empty(), "missing IR/fallback name");
        sourceName |= node.sourceName == "loaded";
        location |=
            node.line > 0 && node.sourceLocation.find("pointer_flow.cpp") != std::string::npos;
        demangled |= node.function == "inspect(Box*, int*, bool)";
        if (!debug) require(node.line == 0, "stripped IR unexpectedly has a source line");
    }
    for (const auto& edge : graph.edges()) {
        require(edge.src.value < graph.nodes().size() && edge.dst.value < graph.nodes().size(),
                "invalid edge endpoint");
        allocation |=
            edge.label == Label::New && graph.nodes()[edge.src.value].kind == NodeKind::Obj;
        store |= edge.label == Label::Store && edge.field == FieldOffset{2};
        load |= edge.label == Label::Load && edge.field == FieldOffset{2};
        gep |= edge.label == Label::Gep && edge.field == FieldOffset{2};
        merge |= edge.label == Label::Assign && edge.dir == CallDir::None &&
                 graph.nodes()[edge.dst.value].function == "inspect(Box*, int*, bool)";
    }
    require(allocation && store && load && gep && merge, "missing allocation/field/GEP/merge flow");
    require(demangled, "function names were not demangled");
    if (debug) require(sourceName && location, "missing debug name or file/line");

    bool direct = false, indirect = false, emptyCall = false, unresolved = false;
    for (std::int32_t id = 0; id < static_cast<std::int32_t>(graph.callSites().size()); ++id) {
        const auto& site = graph.callSites()[id];
        if (startsWith(site.caller, "unresolved(")) {
            unresolved |= site.flags.isIndirect && site.targets.empty();
        }
        for (const auto& target : site.targets) {
            if (target.function == "noArguments()") {
                emptyCall = !site.flags.isIndirect && site.actuals.empty() && !site.actualRet;
                continue;
            }
            if (target.function != "identity(int*)") continue;
            require(site.actuals.size() == 1 && site.actuals[0] && target.formals.size() == 1 &&
                        target.formals[0] && site.actualRet && target.ret,
                    "missing call metadata");
            require(hasEdge(graph, *site.actuals[0], *target.formals[0], CallDir::Enter,
                            CallSiteId{id}),
                    "missing actual/formal edge");
            require(hasEdge(graph, *target.ret, *site.actualRet, CallDir::Exit, CallSiteId{id}),
                    "missing return edge");
            if (site.flags.isIndirect)
                indirect = true;
            else
                direct = true;
            if (debug)
                require(site.line > 0 &&
                            site.sourceLocation.find("pointer_flow.cpp") != std::string::npos,
                        "call site location lost");
        }
    }
    require(direct && indirect && emptyCall && unresolved, "incomplete call-site discovery");
    std::ostringstream dot;
    graph.dumpDot(dot); // deliberately after the LLVM/SVF owners have been released
    require(dot.str().find("store[2]") != std::string::npos &&
                dot.str().find("enter@") != std::string::npos,
            "incomplete DOT export");
}

void checkManualGraph() {
    SimplifiedPAG graph;
    Node object;
    object.kind = NodeKind::Obj;
    object.name = "a\"b\\c\nd";
    const auto from = graph.addNode(object);
    const auto to = graph.addNode(Node{});
    require(from != to && !graph.nodes()[from.value].svfId && !graph.nodes()[to.value].svfId,
            "synthetic nodes were incorrectly interned");
    graph.addEdge({from, to, Label::New});
    require(!graph.edges().back().field && !graph.edges().back().callSite,
            "absent edge metadata was not preserved");
    Node identified;
    identified.svfId = SvfId{std::numeric_limits<std::uint32_t>::max()};
    const auto id = graph.addNode(identified);
    require(graph.addNode(identified) == id && graph.findSvf(*identified.svfId) == id,
            "maximum SVF ID was treated as missing");
    CallSite call;
    require(!call.flags.isIndirect && !call.flags.isVirtual, "call flags must default to zero");
    call.flags.isIndirect = true;
    call.flags.isVirtual = true;
    const auto site = graph.addCallSite(call);
    const auto& flags = graph.callSites()[site.value].flags;
    require(flags.isIndirect && flags.isVirtual, "call flags must coexist");
    call.flags.isIndirect = false;
    require(call.flags.isVirtual, "clearing one flag must preserve the other");
    graph.addEdge({from, to, Label::Gep, FieldOffset{0}, site, CallDir::Enter});
    graph.addEdge({from, to, Label::Gep, kAnyField});
    std::ostringstream dot;
    graph.dumpDot(dot);
    require(dot.str().find("gep[0] enter@0") != std::string::npos &&
                dot.str().find("gep[*]") != std::string::npos,
            "zero IDs/offsets or wildcard fields were lost");
    require(dot.str().find("a\\\"b\\\\c\\nd") != std::string::npos, "DOT escaping failed");
}

} // namespace
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    const std::vector<std::string> inputs{argv[1]};
    SVF::LLVMModuleSet::buildSVFModule(inputs);
    SVF::SVFIRBuilder builder;
    auto* pag = builder.build();
    auto* andersen = SVF::AndersenWaveDiff::createAndersenWaveDiff(pag);
    BuildStats stats;
    const auto graph = buildSimplifiedPAG(*pag, *andersen->getCallGraph(), stats);
    SVF::AndersenWaveDiff::releaseAndersenWaveDiff();
    SVF::SVFIR::releaseSVFIR();
    SVF::LLVMModuleSet::releaseLLVMModuleSet();
    try {
        check(graph, inputs[0].find("no-debug") == std::string::npos);
        checkManualGraph();
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
