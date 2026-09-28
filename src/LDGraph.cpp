#include "ldc/LDGraph.h"

#include <array>
#include <ostream>

namespace ldc
{

namespace
{

const char* const kLabelNames[] = {"new", "assign", "store", "load"};

std::string edgeLabel(const Edge& edge)
{
    std::string text = kLabelNames[static_cast<std::size_t>(edge.label)];
    if (edge.field == kAnyField)
        text += "[*]";
    else if (edge.field != kNoField)
        text += "[" + std::to_string(edge.field) + "]";
    if (edge.dir != CallDir::None)
        text += (edge.dir == CallDir::Enter ? " ^c" : " vc") + std::to_string(edge.callSite);
    return text;
}

std::string escapeDot(const std::string& text)
{
    std::string out;
    for (char c : text)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

} // namespace

NodeId LDGraph::nodeFor(SvfId svfId, NodeKind kind, const std::string& name,
                        const std::string& function, int line)
{
    auto [it, inserted] = bySvf_.emplace(svfId, static_cast<NodeId>(nodes_.size()));
    if (inserted)
        nodes_.push_back(Node{kind, svfId, name, function, line});
    return it->second;
}

CallSiteId LDGraph::addCallSite(const CallSite& callSite)
{
    callSites_.push_back(callSite);
    return static_cast<CallSiteId>(callSites_.size() - 1);
}

void LDGraph::printSummary(std::ostream& os) const
{
    std::array<std::size_t, std::size(kLabelNames)> byLabel{};
    std::size_t enter = 0;
    std::size_t exit = 0;
    for (const Edge& edge : edges_)
    {
        ++byLabel[static_cast<std::size_t>(edge.label)];
        enter += edge.dir == CallDir::Enter;
        exit += edge.dir == CallDir::Exit;
    }
    std::size_t objects = 0;
    for (const Node& node : nodes_)
        objects += node.kind == NodeKind::Obj;

    os << "LDGraph: " << nodes_.size() << " nodes (" << objects << " objects), " << edges_.size()
       << " edges, " << callSites_.size() << " call sites\n";
    for (std::size_t label = 0; label < byLabel.size(); ++label)
        os << "  " << kLabelNames[label] << ": " << byLabel[label] << "\n";
    os << "  enter (^c): " << enter << ", exit (vc): " << exit << "\n";
}

void LDGraph::dumpDot(std::ostream& os) const
{
    os << "digraph LDGraph {\n  rankdir=LR;\n  node [fontname=\"monospace\", fontsize=10];\n"
       << "  edge [fontname=\"monospace\", fontsize=9];\n";
    for (NodeId n = 0; n < nodes_.size(); ++n)
    {
        const Node& node = nodes_[n];
        std::string text = node.kind == NodeKind::Obj ? node.name : node.function + "::" + node.name;
        if (node.line != 0)
            text += " @" + std::to_string(node.line);
        const char* shape = node.kind == NodeKind::Obj ? "box" : "ellipse";
        os << "  n" << n << " [shape=" << shape << ", label=\"" << escapeDot(text) << "\"];\n";
    }
    for (const Edge& edge : edges_)
        os << "  n" << edge.src << " -> n" << edge.dst << " [label=\"" << escapeDot(edgeLabel(edge))
           << "\"];\n";
    os << "}\n";
}

} // namespace ldc
