#include "ldc/P3Ctx.h"

#include <array>
#include <deque>
#include <map>
#include <string>
#include <utility>

namespace ldc {

namespace {

// The DFA of paper Fig. 10 for L_Dr, the regularisation of L_D that ignores fields and treats
// loads and dispatch as assignments: O --new--> flows --assign--> flows --store--> flows‾
// --assign‾--> flows‾ --new‾--> O, with store edges traversable both ways.
enum State { O, Flows, FlowsBar, kStates };
enum Step { New, NewInv, Assign, AssignInv, Store };

struct Pass {
    std::vector<std::vector<std::pair<Step, int>>> out;
    std::vector<std::vector<int>> callersOf; ///< entry node → caller-side receiver nodes
    std::vector<bool> isEntry, summary;      ///< summary: the node's callee pumps values back
    std::vector<std::array<bool, kStates>> seen;
    std::deque<std::pair<int, State>> work;

    int add() {
        out.emplace_back();
        callersOf.emplace_back();
        isEntry.push_back(false);
        summary.push_back(false);
        seen.push_back({});
        return static_cast<int>(out.size()) - 1;
    }
    void edge(int a, Step s, int b) { out[a].push_back({s, b}); }
    void assign(int a, int b) { edge(a, Assign, b), edge(b, AssignInv, a); }
    void store(int value, int base) { edge(value, Store, base), edge(base, Store, value); }
    void newEdge(int object, int var) { edge(object, New, var), edge(var, NewInv, object); }
    void call(int receiver, int entry) {
        isEntry[entry] = true;
        callersOf[entry].push_back(receiver);
    }

    void visit(int n, State q) {
        if (seen[n][q]) return;
        seen[n][q] = true;
        work.push_back({n, q});
    }
    void run() {
        for (int n = 0; n < static_cast<int>(out.size()); ++n)
            if (isEntry[n]) visit(n, Flows); // [F-Init]
        while (!work.empty()) {
            const auto [n, q] = work.front();
            work.pop_front();
            for (const auto& [step, m] : out[n]) { // [F-Propa]
                if (q == O && step == New) visit(m, Flows);
                if (q == Flows && step == Assign) visit(m, Flows);
                if (q == Flows && step == Store) visit(m, FlowsBar);
                if (q == FlowsBar && step == AssignInv) visit(m, FlowsBar);
                if (q == FlowsBar && step == NewInv) visit(m, O);
            }
            if (q == Flows && summary[n]) visit(n, FlowsBar);
            if (q == FlowsBar && isEntry[n]) // [F-Sum]: the callee pumps values back out
                for (int caller : callersOf[n]) {
                    summary[caller] = true;
                    if (seen[caller][Flows]) visit(caller, FlowsBar);
                }
        }
    }
};

bool fig6Edge(const LDGraph& graph, const Edge& e) {
    return e.label == Label::Dispatch || e.dir == CallDir::BoxEnter || e.dir == CallDir::BoxExit ||
           isSyntheticField(e.field) || graph.nodes()[e.src].kind == NodeKind::RecvCopy ||
           graph.nodes()[e.dst].kind == NodeKind::RecvCopy;
}

} // namespace

std::vector<bool> contextInsensitiveNodes(const LDGraph& graph) {
    Pass pass;
    for (std::size_t n = 0; n < graph.nodes().size(); ++n) pass.add();
    std::map<std::string, int> dummyThis;              // function → its dummy receiver formal
    std::map<CallSiteId, std::pair<int, int>> dummyAt; // call site → (receiver, object)
    auto thisOf = [&](const std::string& f) {
        auto [it, fresh] = dummyThis.emplace(f, 0);
        if (fresh) it->second = pass.add();
        return it->second;
    };
    auto receiverAt = [&](CallSiteId c) {
        auto [it, fresh] = dummyAt.emplace(c, std::pair{0, 0});
        if (fresh) {
            it->second = {pass.add(), pass.add()};
            pass.newEdge(it->second.second, it->second.first);
        }
        return it->second.first;
    };

    for (const Edge& e : graph.edges()) {
        if (fig6Edge(graph, e)) continue; // virtual calls are modelled from the call sites below
        if (e.dir != CallDir::None) {
            if (graph.callSites()[static_cast<std::size_t>(e.callSite)].isVirtual) continue;
            const int r = receiverAt(e.callSite);
            if (e.dir == CallDir::Enter) { // a --store--> r --ĉ--> this_g --load--> p
                const int self = thisOf(graph.nodes()[e.dst].function);
                pass.store(static_cast<int>(e.src), r);
                pass.call(r, self);
                pass.assign(self, static_cast<int>(e.dst));
            } else { // ret --store--> this_g;  r --load--> x
                pass.store(static_cast<int>(e.src), thisOf(graph.nodes()[e.src].function));
                pass.assign(r, static_cast<int>(e.dst));
            }
            continue;
        }
        const int a = static_cast<int>(e.src), b = static_cast<int>(e.dst);
        switch (e.label) {
        case Label::New: pass.newEdge(a, b); break;
        case Label::Store: pass.store(a, b); break;
        default: pass.assign(a, b); break; // assign, gep, load
        }
    }
    for (const CallSite& site : graph.callSites()) { // Fig. 6, with dispatch as an assignment
        if (!site.isVirtual || site.actuals.empty() || !site.actuals[0]) continue;
        const int r = static_cast<int>(*site.actuals[0]);
        for (std::size_t i = 1; i < site.actuals.size(); ++i)
            if (site.actuals[i]) pass.store(static_cast<int>(*site.actuals[i]), r);
        if (site.actualRet) pass.assign(r, static_cast<int>(*site.actualRet));
        for (const VirtualTarget& target : site.targets) {
            if (target.formals.empty()) continue;
            const int self = static_cast<int>(target.formals[0]);
            pass.call(r, self);
            for (std::size_t i = 1; i < target.formals.size(); ++i)
                pass.assign(self, static_cast<int>(target.formals[i]));
            if (target.ret) pass.store(static_cast<int>(*target.ret), self);
        }
    }
    pass.run();

    // Eq. 28: context-sensitive iff reached at O, or at both flows and flows‾.
    std::vector<bool> insensitive(graph.nodes().size());
    for (std::size_t n = 0; n < insensitive.size(); ++n) {
        const auto& s = pass.seen[n];
        insensitive[n] = !(s[O] || (s[Flows] && s[FlowsBar]));
    }
    for (const Edge& e : graph.edges()) // r#c lives where r lives
        if (graph.nodes()[e.dst].kind == NodeKind::RecvCopy && e.label == Label::Assign)
            insensitive[e.dst] = insensitive[e.src];
    return insensitive;
}

} // namespace ldc
