#include "CflBackends.h"

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace ldc::cfl {
namespace {

bool isFamily(const std::string& name) {
    return name.size() > 2 && name.compare(name.size() - 2, 2, "_i") == 0;
}

std::string baseOf(const std::string& name) {
    return isFamily(name) ? name.substr(0, name.size() - 2) : name;
}

// A worklist CFL-reachability solver that keeps each fact's first derivation, so that the path of
// a fact can be read back: a terminal fact is an edge, a derived one joins the facts of its rule's
// right-hand side, which were all derived before it.
class PathSolver {
public:
    PathSolver(const Graph& graph, const Grammar& grammar) : graph_(graph) {
        for (const auto& rule : grammar) {
            const Rule r{id(rule.lhs), id(rule.a), rule.b.empty() ? -1 : id(rule.b),
                         isFamily(rule.lhs), isFamily(rule.a), isFamily(rule.b)};
            byA_[r.a].push_back(rules_.size());
            if (r.b >= 0) byB_[r.b].push_back(rules_.size());
            rules_.push_back(r);
        }
    }

    std::vector<std::optional<std::vector<std::size_t>>> run(const std::string& start,
                                                             const Pairs& queries) {
        std::vector<std::optional<std::vector<std::size_t>>> result(queries.size());
        const auto it = ids_.find(start);
        if (it == ids_.end()) return result;
        for (std::size_t e = 0; e < graph_.edges.size(); ++e) {
            const auto& edge = graph_.edges[e];
            if (const auto s = ids_.find(edge.label); s != ids_.end())
                add({s->second, edge.index, edge.src, edge.dst}, {-1, e, 0, 0});
        }
        while (!queue_.empty()) {
            const auto f = queue_.front();
            queue_.pop_front();
            const auto fact = facts_[f];
            for (auto r : byA_[fact.symbol]) {
                const auto& rule = rules_[r];
                if (rule.b < 0) {
                    if (const auto i = bind(rule, fact.index, -1))
                        add({rule.lhs, *i, fact.u, fact.v}, {static_cast<int>(r), 0, f, 0});
                    continue;
                }
                auto& next = from_[rule.b][fact.v];
                for (std::size_t k = 0; k < next.size(); ++k) {
                    const auto g = next[k];
                    if (const auto i = bind(rule, fact.index, facts_[g].index))
                        add({rule.lhs, *i, fact.u, facts_[g].v}, {static_cast<int>(r), 0, f, g});
                }
            }
            for (auto r : byB_[fact.symbol]) {
                const auto& rule = rules_[r];
                auto& previous = to_[rule.a][fact.u];
                for (std::size_t k = 0; k < previous.size(); ++k) {
                    const auto g = previous[k];
                    if (const auto i = bind(rule, facts_[g].index, fact.index))
                        add({rule.lhs, *i, facts_[g].u, fact.v}, {static_cast<int>(r), 0, g, f});
                }
            }
        }
        for (std::size_t q = 0; q < queries.size(); ++q) {
            const auto [u, v] = queries[q];
            const auto f = known_.lower_bound({it->second, u, v, INT32_MIN});
            if (f != known_.end() && std::get<0>(f->first) == it->second &&
                std::get<1>(f->first) == u && std::get<2>(f->first) == v)
                result[q] = edgesOf(f->second);
        }
        return result;
    }

private:
    struct Rule {
        int lhs, a, b;
        bool familyLhs, familyA, familyB;
    };
    // symbol(u, v), index: within the symbol's family, or -1.
    struct Fact {
        int symbol;
        std::int32_t index;
        std::uint32_t u, v;
    };
    // rule -1: the fact is graph edge `edge`; otherwise facts a and b (a alone for a unit rule).
    struct Derivation {
        int rule;
        std::size_t edge, a, b;
    };

    int id(const std::string& name) {
        return ids_.emplace(baseOf(name), static_cast<int>(ids_.size())).first->second;
    }

    // The rule's index, the same for all its families; its left-hand side's index, or nullopt if
    // the two facts disagree on it.
    static std::optional<std::int32_t> bind(const Rule& rule, std::int32_t a, std::int32_t b) {
        std::optional<std::int32_t> index;
        if (rule.familyA) index = a;
        if (rule.familyB) {
            if (index && *index != b) return std::nullopt;
            index = b;
        }
        if (!rule.familyLhs) return -1;
        return index;
    }

    void add(const Fact& fact, const Derivation& derivation) {
        const auto key = std::tuple{fact.symbol, fact.u, fact.v, fact.index};
        if (!known_.emplace(key, facts_.size()).second) return;
        from_[fact.symbol][fact.u].push_back(facts_.size());
        to_[fact.symbol][fact.v].push_back(facts_.size());
        queue_.push_back(facts_.size());
        facts_.push_back(fact);
        derivations_.push_back(derivation);
    }

    std::vector<std::size_t> edgesOf(std::size_t fact) const {
        std::vector<std::size_t> edges;
        std::vector<std::size_t> stack{fact};
        while (!stack.empty()) {
            const auto& d = derivations_[stack.back()];
            stack.pop_back();
            if (d.rule < 0) {
                edges.push_back(d.edge);
                continue;
            }
            if (rules_[d.rule].b >= 0) stack.push_back(d.b);
            stack.push_back(d.a);
        }
        return edges;
    }

    const Graph& graph_;
    std::map<std::string, int> ids_;
    std::vector<Rule> rules_;
    std::unordered_map<int, std::vector<std::size_t>> byA_, byB_;
    std::vector<Fact> facts_;
    std::vector<Derivation> derivations_;
    std::map<std::tuple<int, std::uint32_t, std::uint32_t, std::int32_t>, std::size_t> known_;
    std::unordered_map<int, std::unordered_map<std::uint32_t, std::vector<std::size_t>>> from_, to_;
    std::deque<std::size_t> queue_;
};

} // namespace

std::vector<std::optional<std::vector<std::size_t>>>
findPaths(const Graph& graph, const Grammar& grammar, const std::string& start, const Pairs& queries) {
    return PathSolver(graph, grammar).run(start, queries);
}

} // namespace ldc::cfl
