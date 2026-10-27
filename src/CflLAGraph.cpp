#include "CflBackends.h"

extern "C" {
#include <GraphBLAS.h>
#include <LAGraph.h>
#include <LAGraphX.h>
}

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace ldc::cfl {
namespace {

// Drop empty matrices, pick matrix formats, multiply lazily, store families as block matrices.
constexpr std::int8_t kOptimizations = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3);

void check(GrB_Info info, const char* what, const char* msg = "") {
    if (info != GrB_SUCCESS && info != GrB_NO_VALUE)
        throw std::runtime_error(std::string(what) + " failed (" + std::to_string(info) + ") " +
                                 msg);
}

void initLAGraph() {
    static const bool initialized = [] {
        char msg[LAGRAPH_MSG_LEN];
        check(static_cast<GrB_Info>(LAGraph_Init(msg)), "LAGraph_Init", msg);
        return true;
    }();
    (void)initialized;
}

bool isFamily(const std::string& name) {
    return name.size() > 2 && name.compare(name.size() - 2, 2, "_i") == 0;
}

std::string baseOf(const std::string& name) {
    return isFamily(name) ? name.substr(0, name.size() - 2) : name;
}

// Index counts of families: one more than the largest index of an edge with the family's
// label, at least 1. LAGraph's indexed rule has one count for all its families, so families in
// one rule get the largest of their counts.
std::map<std::string, std::uint32_t> familyCounts(const Graph& graph, const Grammar& grammar) {
    std::map<std::string, std::uint32_t> counts;
    for (const auto& rule : grammar)
        for (const auto* name : {&rule.lhs, &rule.a, &rule.b})
            if (isFamily(*name)) counts.emplace(baseOf(*name), 1);
    for (const auto& e : graph.edges)
        if (auto it = counts.find(e.label); it != counts.end() && e.index >= 0)
            it->second = std::max(it->second, static_cast<std::uint32_t>(e.index) + 1);
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& rule : grammar) {
            std::uint32_t count = 0;
            for (const auto* name : {&rule.lhs, &rule.a, &rule.b})
                if (isFamily(*name)) count = std::max(count, counts[baseOf(*name)]);
            for (const auto* name : {&rule.lhs, &rule.a, &rule.b})
                if (isFamily(*name) && counts[baseOf(*name)] < count) {
                    counts[baseOf(*name)] = count;
                    changed = true;
                }
        }
    }
    return counts;
}

// Symbol ids: a family takes `count` consecutive ids.
class Symbols {
public:
    explicit Symbols(std::map<std::string, std::uint32_t> counts) : counts_(std::move(counts)) {}

    struct Symbol {
        std::int32_t id;
        std::uint32_t count; // 0: not a family
    };

    Symbol get(const std::string& name) {
        const auto base = baseOf(name);
        if (auto it = ids_.find(base); it != ids_.end()) return it->second;
        const auto count = isFamily(name) ? counts_.at(base) : 0;
        const Symbol symbol{static_cast<std::int32_t>(size_), count};
        size_ += std::max<std::uint32_t>(count, 1);
        return ids_.emplace(base, symbol).first->second;
    }

    std::optional<std::int32_t> find(const std::string& base, std::int32_t index) const {
        auto it = ids_.find(base);
        if (it == ids_.end()) return std::nullopt;
        if (it->second.count == 0) return it->second.id;
        if (index < 0 || static_cast<std::uint32_t>(index) >= it->second.count) return std::nullopt;
        return it->second.id + index;
    }

    std::size_t size() const { return size_; }

private:
    std::map<std::string, std::uint32_t> counts_;
    std::map<std::string, Symbol> ids_;
    std::size_t size_ = 0;
};

LAGraph_rule_EWCNF toEwcnf(const Rule& rule, Symbols& symbols) {
    LAGraph_rule_EWCNF out{};
    const auto use = [&](const std::string& name, std::uint8_t bit) -> std::int32_t {
        if (name.empty()) return -1;
        const auto symbol = symbols.get(name);
        if (symbol.count != 0) {
            out.indexed |= bit;
            out.indexed_count = symbol.count;
        }
        return symbol.id;
    };
    out.nonterm = use(rule.lhs, LAGraph_EWNCF_INDEX_NONTERM);
    out.prod_A = use(rule.a, LAGraph_EWNCF_INDEX_PROD_A);
    out.prod_B = use(rule.b, LAGraph_EWNCF_INDEX_PROD_B);
    return out;
}

} // namespace

std::map<std::string, Pairs> solveWithLAGraph(const Graph& graph, const Grammar& grammar) {
    initLAGraph();
    Symbols symbols(familyCounts(graph, grammar));
    std::vector<LAGraph_rule_EWCNF> rules;
    for (const auto& rule : grammar) rules.push_back(toEwcnf(rule, symbols));

    const auto n = static_cast<GrB_Index>(graph.nodes);
    std::vector<std::vector<GrB_Index>> rows(symbols.size()), cols(symbols.size());
    for (const auto& e : graph.edges)
        if (const auto symbol = symbols.find(e.label, e.index)) {
            rows[*symbol].push_back(e.src);
            cols[*symbol].push_back(e.dst);
        }

    std::vector<GrB_Matrix> adj(symbols.size(), nullptr), out(symbols.size(), nullptr);
    const auto release = [&] {
        for (auto& m : adj) GrB_Matrix_free(&m);
        for (auto& m : out) GrB_Matrix_free(&m);
    };
    std::map<std::string, Pairs> result;
    try {
        for (std::size_t s = 0; s < symbols.size(); ++s) {
            check(GrB_Matrix_new(&adj[s], GrB_BOOL, n, n), "GrB_Matrix_new");
            check(GrB_Matrix_new(&out[s], GrB_BOOL, n, n), "GrB_Matrix_new");
            if (rows[s].empty()) continue;
            const std::unique_ptr<bool[]> ones(new bool[rows[s].size()]);
            std::fill_n(ones.get(), rows[s].size(), true);
            check(GrB_Matrix_build_BOOL(adj[s], rows[s].data(), cols[s].data(), ones.get(),
                                        rows[s].size(), GrB_LOR),
                  "GrB_Matrix_build");
        }
        char msg[LAGRAPH_MSG_LEN] = "";
        check(LAGraph_CFL_reachability_adv(out.data(), adj.data(), symbols.size(), rules.data(),
                                           rules.size(), msg, kOptimizations),
              "LAGraph_CFL_reachability_adv", msg);
        std::set<std::string> nonterminals;
        for (const auto& rule : grammar) nonterminals.insert(rule.lhs);
        for (const auto& name : nonterminals) {
            const auto symbol = symbols.get(name);
            for (std::uint32_t k = 0; k < std::max<std::uint32_t>(symbol.count, 1); ++k) {
                const auto m = out[symbol.id + k];
                auto& pairs = result[symbol.count ? name + "[" + std::to_string(k) + "]" : name];
                GrB_Index count = 0;
                check(GrB_Matrix_nvals(&count, m), "GrB_Matrix_nvals");
                std::vector<GrB_Index> from(count), to(count);
                check(GrB_Matrix_extractTuples_BOOL(from.data(), to.data(), nullptr, &count, m),
                      "GrB_Matrix_extractTuples");
                for (GrB_Index i = 0; i < count; ++i)
                    pairs.push_back(
                        {static_cast<std::uint32_t>(from[i]), static_cast<std::uint32_t>(to[i])});
            }
        }
    } catch (...) {
        release();
        throw;
    }
    release();
    return result;
}

} // namespace ldc::cfl
