#include "CflBackends.h"

#include "CFL/CFGNormalizer.h"
#include "CFL/CFLSolver.h"
#include "CFL/GrammarBuilder.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace ldc::cfl {
namespace {

bool isFamily(const std::string& name) {
    return name.size() > 2 && name.compare(name.size() - 2, 2, "_i") == 0;
}

std::string baseOf(const std::string& name) {
    return isFamily(name) ? name.substr(0, name.size() - 2) : name;
}

// SVF reads a symbol's kind up to the first '_' (the rest is its attribute) and tells
// nonterminals from terminals by an upper-case first letter, so every symbol gets a generated
// name: "N<k>" for a nonterminal, "t<k>" for a terminal, "_i" kept for a family.
class Names {
public:
    explicit Names(const Grammar& grammar) {
        for (const auto& rule : grammar) nonterminals_.insert(baseOf(rule.lhs));
        for (const auto& rule : grammar)
            for (const auto* name : {&rule.lhs, &rule.a, &rule.b})
                if (!name->empty()) get(*name);
    }

    // The SVF name of a grammar symbol (with "_i" for a family).
    std::string get(const std::string& name) {
        const auto base = baseOf(name);
        auto it = names_.find(base);
        if (it == names_.end()) {
            const bool nonterminal = nonterminals_.count(base);
            it = names_.emplace(base, (nonterminal ? "N" : "t") + std::to_string(names_.size()))
                     .first;
            (nonterminal ? svfNonterminals_ : svfTerminals_).push_back(it->second);
        }
        return isFamily(name) ? it->second + "_i" : it->second;
    }

    // The SVF name of a base name, if the grammar uses it.
    const std::string* find(const std::string& base) const {
        auto it = names_.find(base);
        return it == names_.end() ? nullptr : &it->second;
    }

    const std::vector<std::string>& terminals() const { return svfTerminals_; }

private:
    std::set<std::string> nonterminals_;
    std::map<std::string, std::string> names_;
    std::vector<std::string> svfTerminals_, svfNonterminals_;
};

// The grammar in SVF's text format: start symbol (any: all nonterminals are solved), terminals,
// then `lhs -> a b;` productions.
std::filesystem::path writeGrammar(const Grammar& grammar, Names& names) {
    static int counter = 0;
    const auto path =
        std::filesystem::temp_directory_path() /
        ("ldc-cfl-" + std::to_string(getpid()) + "-" + std::to_string(counter++) + ".txt");
    std::ofstream out(path);
    out << "Start:\n" << names.get(baseOf(grammar.front().lhs)) << "\nTerminal:\n";
    for (const auto& t : names.terminals()) out << t << " ";
    out << "\nProductions:\n";
    for (const auto& rule : grammar)
        out << names.get(rule.lhs) << " -> " << names.get(rule.a)
            << (rule.b.empty() ? "" : " " + names.get(rule.b)) << ";\n";
    if (!out) throw std::runtime_error("Cannot write CFL grammar: " + path.string());
    return path;
}

} // namespace

std::map<std::string, Pairs> solveWithSvf(const Graph& graph, const Grammar& grammar) {
    using SVF::CFGrammar;
    if (grammar.empty()) return {};
    Names names(grammar);
    const auto path = writeGrammar(grammar, names);
    std::unique_ptr<SVF::GrammarBase> base(SVF::GrammarBuilder(path.string()).build());
    std::filesystem::remove(path);

    // Edges carry their label's kind and, for a family, the index as its attribute.
    auto graphPtr = std::make_unique<SVF::CFLGraph>(base->getStartKind());
    for (std::uint32_t n = 0; n < graph.nodes; ++n) graphPtr->addCFLNode(n, new SVF::CFLNode(n));
    const auto& terminals = base->getTerminals();
    for (const auto& e : graph.edges) {
        const auto* name = names.find(e.label);
        const auto it = name ? terminals.find(*name) : terminals.end();
        if (it == terminals.end()) continue; // a label the grammar does not use
        CFGrammar::Symbol symbol;
        symbol.kind = it->second;
        if (e.index >= 0) {
            symbol.attribute = static_cast<CFGrammar::Attribute>(e.index);
            base->insertAttribute(it->second, symbol.attribute);
        }
        graphPtr->addCFLEdge(graphPtr->getGNode(e.src), graphPtr->getGNode(e.dst), symbol);
    }
    if (base->getTotalKind() > 255)
        throw std::runtime_error("CFL grammar has more symbols than SVF supports (255)");

    std::unique_ptr<CFGrammar> normal(SVF::CFGNormalizer().normalize(base.get()));
    // The solver owns and deletes the graph and the normalized grammar.
    const auto* cflGraph = graphPtr.get();
    auto* cflGrammar = normal.get();
    SVF::CFLSolver solver(graphPtr.release(), normal.release());
    solver.solve();

    // Our nonterminals by SVF kind, with whether each is a family.
    std::map<CFGrammar::Kind, std::pair<std::string, bool>> byKind;
    std::map<std::string, Pairs> result;
    for (const auto& rule : grammar) {
        const auto kind = cflGrammar->getNonterminals().at(*names.find(baseOf(rule.lhs)));
        byKind[kind] = {rule.lhs, isFamily(rule.lhs)};
        if (!isFamily(rule.lhs)) result[rule.lhs];
    }
    for (const auto* edge : cflGraph->getCFLEdges()) {
        const CFGrammar::Symbol symbol = edge->getEdgeKind();
        const auto it = byKind.find(symbol.kind);
        if (it == byKind.end()) continue; // a terminal, or a nonterminal SVF's normalizer added
        const auto& [name, family] = it->second;
        if (!family && symbol.attribute != 0) continue;
        const auto key = family ? name + "[" + std::to_string(symbol.attribute) + "]" : name;
        result[key].push_back({edge->getSrcID(), edge->getDstID()});
    }
    return result;
}

} // namespace ldc::cfl
