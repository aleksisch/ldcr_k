#include "ldc/SimplifiedPAG.h"
#include <array>
#include <ostream>

std::size_t std::hash<ldc::frontend::SvfId>::operator()(ldc::frontend::SvfId id) const noexcept {
    return std::hash<std::uint32_t>{}(id.value);
}

namespace ldc::frontend {
namespace {
const char* const labels[] = {"new", "assign", "store", "load", "gep"};
std::string escapeDot(const std::string& text) {
    std::string out;
    for (auto c : text) {
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

NodeId SimplifiedPAG::addNode(const Node& node) {
    if (node.svfId)
        if (auto id = findSvf(*node.svfId)) return *id;
    const auto id = NodeId{static_cast<std::uint32_t>(nodes_.size())};
    nodes_.push_back(node);
    if (node.svfId) bySvf_.emplace(*node.svfId, id);
    return id;
}

std::optional<NodeId> SimplifiedPAG::findSvf(SvfId id) const {
    auto it = bySvf_.find(id);
    return it == bySvf_.end() ? std::nullopt : std::optional<NodeId>(it->second);
}

CallSiteId SimplifiedPAG::addCallSite(const CallSite& site) {
    sites_.push_back(site);
    return CallSiteId{static_cast<std::int32_t>(sites_.size() - 1)};
}

void SimplifiedPAG::addEdge(const Edge& edge) { edges_.push_back(edge); }

const std::vector<Node>& SimplifiedPAG::nodes() const { return nodes_; }

const std::vector<Edge>& SimplifiedPAG::edges() const { return edges_; }

const std::vector<CallSite>& SimplifiedPAG::callSites() const { return sites_; }

void SimplifiedPAG::printSummary(std::ostream& out) const {
    out << "SimplifiedPAG: " << nodes_.size() << " nodes, " << edges_.size() << " edges, "
        << sites_.size() << " call sites\n";
    std::array<std::size_t, 5> counts{};
    for (const auto& edge : edges_) ++counts[static_cast<std::size_t>(edge.label)];
    for (std::size_t i = 0; i < counts.size(); ++i)
        out << "  " << labels[i] << ": " << counts[i] << "\n";
}

void SimplifiedPAG::dumpDot(std::ostream& out) const {
    out << "digraph SimplifiedPAG {\n  rankdir=LR;\n";
    for (std::size_t id = 0; id < nodes_.size(); ++id) {
        const auto& node = nodes_[id];
        std::string text = node.function.empty() ? "" : node.function + "::";
        text += node.sourceName.empty() ? node.name : node.sourceName;
        if (node.line) text += " @" + std::to_string(node.line);
        out << "  n" << id << " [shape=" << (node.kind == NodeKind::Obj ? "box" : "ellipse")
            << ", label=\"" << escapeDot(text) << "\"";
        if (node.svfId) out << ", svf_id=\"" << node.svfId->value << "\"";
        out << ", source=\"" << escapeDot(node.sourceLocation) << "\"];\n";
    }
    for (const auto& edge : edges_) {
        std::string text = labels[static_cast<std::size_t>(edge.label)];
        if (edge.field == kAnyField)
            text += "[*]";
        else if (edge.field)
            text += "[" + std::to_string(edge.field->value) + "]";
        if (edge.dir != CallDir::None && edge.callSite)
            text += (edge.dir == CallDir::Enter ? " enter@" : " exit@") +
                    std::to_string(edge.callSite->value);
        out << "  n" << edge.src.value << " -> n" << edge.dst.value << " [label=\""
            << escapeDot(text) << "\"];\n";
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
