#include "PagDot.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <tuple>
#include <utility>

namespace ldc {
namespace {

using frontend::CallDir;
using frontend::Label;

const char* const labels[] = {"new",   "assign", "store",    "load", "gep",    "dispatch",
                              "shift", "cell",   "starCell", "hub",  "cellHub"};

std::string escape(const std::string& text) {
    std::string out;
    for (auto c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// The source file, one left-justified line each, numbered.
std::string numbered(const std::string& path) {
    std::ifstream in(path);
    std::string text, line;
    for (int number = 1; std::getline(in, line); ++number) {
        auto prefix = std::to_string(number);
        text += std::string(prefix.size() < 3 ? 3 - prefix.size() : 0, ' ') + prefix + "  ";
        text += escape(line) + "\\l";
    }
    return text;
}

// Text with each of its lines left-justified.
std::string lines(const std::string& text) {
    std::string out;
    for (auto c : text) out += c == '\n' ? "\\l" : escape(std::string(1, c));
    return out;
}

std::string html(const std::string& text) {
    std::string out;
    for (auto c : text) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out += c;
    }
    return out;
}

} // namespace

std::string edgeTuple(const frontend::SimplifiedPAG& pag, const frontend::Edge& edge) {
    std::string above = labels[static_cast<std::size_t>(edge.label)];
    if (edge.field == frontend::kAnyField)
        above += "[*]";
    else if (edge.field && edge.field->value <= -10)
        above += "[" + std::to_string(-10 - edge.field->value) + "]";
    else if (edge.field)
        above += "[" + std::to_string(edge.field->value) + "]";
    if (edge.type) above += "[" + pag.nodes()[edge.type->value].name + "]";
    if (edge.reversed) above += "_r";
    std::string below = "ε";
    if (edge.dir != CallDir::None && edge.callSite) {
        const bool enter = (edge.dir == CallDir::Enter || edge.dir == CallDir::DispatchEnter) !=
                           edge.reversed;
        const bool boxed = edge.dir == CallDir::DispatchEnter || edge.dir == CallDir::DispatchExit;
        below = std::string(boxed ? "⟦" : "") + (enter ? "ĉ" : "č") +
                std::to_string(edge.callSite->value) + (boxed ? "⟧" : "");
    }
    return "(" + above + ", " + below + ")";
}

void writePagDot(std::ostream& out, const frontend::SimplifiedPAG& pag, const std::string& code,
                 const std::string& legend, const std::vector<DrawnPath>& paths,
                 const frontend::SimplifiedPAG* original) {
    out << "digraph PAG {\n  rankdir=LR;\n";
    if (!code.empty() || !legend.empty())
        out << "  labelloc=t;\n  labeljust=r;\n  fontname=\"monospace\";\n  label=\""
            << (code.empty() ? "" : numbered(code) + "\\l") << lines(legend) << "\";\n";
    for (std::size_t id = 0; id < pag.nodes().size(); ++id) {
        const auto& node = pag.nodes()[id];
        std::string text = node.function.empty() ? "" : node.function + "::";
        text += node.sourceName.empty() ? node.name : node.sourceName;
        if (node.line) text += " @" + std::to_string(node.line);
        out << "  n" << id << " [shape=" << (node.kind == frontend::NodeKind::Obj ? "box" : "ellipse")
            << ", label=\"" << escape(text) << "\"];\n";
    }
    // The steps of the paths on each forward edge: a reversed edge's steps go on its forward edge,
    // marked "←".
    const auto key = [](const frontend::Edge& e, bool swap) {
        return std::tuple{swap ? e.dst.value : e.src.value, swap ? e.src.value : e.dst.value,
                          e.label, e.field ? e.field->value : INT32_MIN,
                          e.callSite ? e.callSite->value : INT32_MIN, e.dir,
                          e.type ? e.type->value : UINT32_MAX};
    };
    std::map<decltype(key(pag.edges().front(), false)), std::size_t> forward;
    for (std::size_t e = 0; e < pag.edges().size(); ++e)
        if (!pag.edges()[e].reversed) forward.emplace(key(pag.edges()[e], false), e);
    std::map<std::size_t, std::vector<std::pair<std::string, std::string>>> steps;
    for (const auto& path : paths)
        for (std::size_t step = 0; step < path.edges.size(); ++step) {
            const auto& edge = pag.edges()[path.edges[step]];
            const auto it = forward.find(key(edge, edge.reversed));
            if (it == forward.end()) continue;
            steps[it->second].push_back(
                {path.color, path.name + std::to_string(step + 1) + (edge.reversed ? "←" : "")});
        }
    for (std::size_t e = 0; e < pag.edges().size(); ++e) {
        const auto& edge = pag.edges()[e];
        if (edge.reversed) continue;
        out << "  n" << edge.src.value << " -> n" << edge.dst.value;
        const auto it = steps.find(e);
        if (it == steps.end()) {
            out << " [label=\"" << escape(edgeTuple(pag, edge)) << "\"];\n";
            continue;
        }
        // One parallel line per color, the steps below the tuple in their colors.
        std::vector<std::string> colors;
        std::string label = html(edgeTuple(pag, edge)) + "<br/>";
        for (const auto& [color, text] : it->second) {
            if (std::find(colors.begin(), colors.end(), color) == colors.end())
                colors.push_back(color);
            label += "<font color=\"" + color + "\">" + html(text) + "</font> ";
        }
        std::string color;
        for (const auto& c : colors) color += (color.empty() ? "" : ":") + c;
        out << " [color=\"" << color << "\", penwidth=2, label=<" << label << ">];\n";
    }
    if (original)
        for (const auto& edge : original->edges())
            if (!edge.reversed && !forward.count(key(edge, false)))
                out << "  n" << edge.src.value << " -> n" << edge.dst.value
                    << " [color=gray, fontcolor=gray, style=dashed, label=\""
                    << escape(edgeTuple(*original, edge)) << "\"];\n";
    out << "}\n";
}

} // namespace ldc
