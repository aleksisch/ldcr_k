#include "Facts.h"

#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace ldc {
namespace {

// "E::foo(G*)" -> "E::foo".
std::string functionKey(const std::string& name) { return name.substr(0, name.find('(')); }

// Source line -> label, for lines with a `// <label>` comment.
std::unordered_map<int, std::string> lineLabels(const std::string& path) {
    std::unordered_map<int, std::string> labels;
    if (path.empty()) return labels;
    std::ifstream in(path);
    std::string line, label;
    for (auto number = 1; std::getline(in, line); ++number)
        if (auto c = line.find("//"); c != std::string::npos)
            if (std::istringstream(line.substr(c + 2)) >> label) labels.emplace(number, label);
    return labels;
}

std::vector<std::string> nodeTexts(const frontend::SimplifiedPAG& graph,
                                   const std::string& source) {
    const auto labels = lineLabels(source);
    std::vector<std::string> texts;
    for (const auto& node : graph.nodes()) {
        if (node.kind == frontend::NodeKind::Obj) {
            const auto it = labels.find(node.line);
            texts.push_back(it != labels.end() ? it->second : node.name);
        } else if (!node.sourceName.empty())
            texts.push_back(functionKey(node.function) + "::" + node.sourceName);
        else
            texts.push_back(functionKey(node.function) + "::" + node.name + "@" +
                            std::to_string(node.line));
    }
    return texts;
}

} // namespace

void writeFacts(std::ostream& out, const frontend::SimplifiedPAG& graph, const PointsTo& facts,
                const std::string& source) {
    const auto texts = nodeTexts(graph, source);
    std::set<std::string> lines;
    for (const auto& [var, objects] : facts)
        for (auto object : objects) lines.insert(texts[var] + " -> " + texts[object]);
    for (const auto& line : lines) out << line << "\n";
}

std::vector<std::string> nodeNames(const frontend::SimplifiedPAG& graph, const std::string& source) {
    return nodeTexts(graph, source);
}

} // namespace ldc
