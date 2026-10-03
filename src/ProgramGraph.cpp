#include "ldc/ProgramGraph.h"
#include <array>
#include <ostream>

namespace ldc::frontend {
namespace {
const char* const labels[] = {"new", "assign", "store", "load", "gep"};
std::string escapeDot(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c;
        }
    }
    return out;
}
} // namespace

NodeId ProgramGraph::addNode(const Node& node) {
    if (node.svfId != kNoSvfId)
        if (auto id = findSvf(node.svfId)) return *id;
    const NodeId id = static_cast<NodeId>(nodes_.size());
    nodes_.push_back(node);
    if (node.svfId != kNoSvfId) bySvf_.emplace(node.svfId, id);
    return id;
}
std::optional<NodeId> ProgramGraph::findSvf(SvfId id) const {
    auto it = bySvf_.find(id);
    return it == bySvf_.end() ? std::nullopt : std::optional<NodeId>(it->second);
}
CallSiteId ProgramGraph::addCallSite(const CallSite& site) {
    sites_.push_back(site);
    return static_cast<CallSiteId>(sites_.size() - 1);
}
void ProgramGraph::printSummary(std::ostream& out) const {
    out << "ProgramGraph: " << nodes_.size() << " nodes, " << edges_.size() << " edges, "
        << sites_.size() << " call sites\n";
    std::array<std::size_t, 5> counts{};
    for (const auto& edge : edges_) ++counts[static_cast<std::size_t>(edge.label)];
    for (std::size_t i = 0; i < counts.size(); ++i)
        out << "  " << labels[i] << ": " << counts[i] << "\n";
}
void ProgramGraph::dumpDot(std::ostream& out) const {
    out << "digraph ProgramGraph {\n  rankdir=LR;\n";
    for (NodeId id = 0; id < nodes_.size(); ++id) {
        const Node& node = nodes_[id];
        std::string text = node.function.empty() ? "" : node.function + "::";
        text += node.sourceName.empty() ? node.name : node.sourceName;
        if (node.line) text += " @" + std::to_string(node.line);
        out << "  n" << id << " [shape=" << (node.kind == NodeKind::Obj ? "box" : "ellipse")
            << ", label=\"" << escapeDot(text) << "\", svf_id=\"" << node.svfId << "\", source=\""
            << escapeDot(node.sourceLocation) << "\"];\n";
    }
    for (const Edge& edge : edges_) {
        std::string text = labels[static_cast<std::size_t>(edge.label)];
        if (edge.field == kAnyField)
            text += "[*]";
        else if (edge.field != kNoField)
            text += "[" + std::to_string(edge.field) + "]";
        if (edge.dir != CallDir::None)
            text +=
                (edge.dir == CallDir::Enter ? " enter@" : " exit@") + std::to_string(edge.callSite);
        out << "  n" << edge.src << " -> n" << edge.dst << " [label=\"" << escapeDot(text)
            << "\"];\n";
    }
    for (std::size_t id = 0; id < sites_.size(); ++id) {
        const auto& site = sites_[id];
        std::string text = "call " + std::to_string(id) + ": " + site.caller;
        for (const auto& target : site.targets) text += "\n-> " + target.function;
        out << "  c" << id << " [shape=note, label=\"" << escapeDot(text) << "\", source=\""
            << escapeDot(site.sourceLocation) << "\"];\n";
    }
    out << "}\n";
}
} // namespace ldc::frontend
