# L_DC for C++ — Plan

Prototype of the CFL-reachability formulation from
He, Lu, Xue, *CFL-Reachability with On-The-Fly Call Graph Construction*, ECOOP 2024
(local copy: `../papers/p3ctx-ecoop24.pdf`), applied to **C++ programs**.

Phase 1 (this plan, in detail): **L_DC_k = L_D ∩ C_k**, where
- **L_D** is kept exact (context-free): field matching + typed dispatch (paper Eq. 8);
- **C_k** is L_C regularized: a finite automaton over k-limited call strings,
  aligned with kCFA's truncation (paper Fig. 1).

L_D ∩ regular = one context-free language, so the problem is decidable and polynomial
for fixed k.

Phase 2 (later, sketched only): add L_R → **L_DCR_k**.

---

## 1. Why L_DC first

| | What it fixes | What it still gets wrong |
|---|---|---|
| L_FC (baseline, = SVF `CFLAlias`, Soot `DemandCSPointsTo`) | fields, contexts | virtual calls: receiver objects cross to the wrong target (paper Fig. 5) |
| **L_DC_k** (phase 1) | + receiver → only its own target (Lemma 3), sound parameter passing (Lemma 4) | dispatch excursion may return under the wrong context (paper Eq. 15) |
| L_DCR_k (phase 2) | + excursion returns the same way (DP-C1, DP-C2) | — (target: equal to kCFA) |

L_DC is sound but not precise (paper Def. 2, Lemma 4). That is fine for phase 1: it already
beats L_FC on the Fig. 5 case, and the Eq. 15 case becomes the motivating test for phase 2.

---

## 2. Formal target for phase 1

### 2.1 Graph (PAG), built from C++ IR

Node kinds: variables (SVF value nodes), abstract objects (allocation sites), per-callsite
receiver copies `r#c`.

Edge labels (each edge also has an inverse `ℓ‾`):

| Label (above edge) | Below-edge | From |
|---|---|---|
| `new[t]` | — | allocation site of dynamic type `t` → pointer |
| `assign` | — | copy / cast / phi |
| `store[f]`, `load[f]` | — | field write / read (`f` = field offset path) |
| `assign` | `ĉ` / `č` | static & indirect calls without receiver: arg → param (ĉ), ret → lhs (č) |
| `store[i]` | `ĉ` | virtual call: argument `a_i` → receiver `r` (synthetic field `i`) |
| `load[i]` | — | callee: `this^m` → parameter `p_i` |
| `store[0]` / `load[0]` | — / `č` | callee `ret^m` → `this^m`; caller `r` → lhs |
| `assign` | — | `r` → `r#c` |
| `dispatch[t]` | `ĉ` | `r#c` → `this^m'`, for each candidate `t`, `m' = dispatch(c, t)` |

These are the rules of paper Fig. 6 ([C-New], [C-Param], [C-Ret], [C-VCall]).
Reference implementation of the same edge shapes (Java): `../p3ctx/src/p3ctx/P3ctxBase.java`,
`buildTransEdges()`.

### 2.2 Language L_D (paper Eq. 8)

```
flowsto   → new[t] ( flows | dispatch[t] )*
flows     → assign | store[f] alias load[f]
alias     → flowsto‾ flowsto
flowsto‾  → ( dispatch[t]‾ | flows‾ )* new[t]‾
flows‾    → assign‾ | load[f]‾ alias store[f]‾
```

`t` ranges over finitely many types, `f` over finitely many fields (incl. synthetic `0..n`).
Implementation: index nonterminals by `t` (and match `f` pairwise), i.e. a finite expansion.

### 2.3 Regular context automaton C_k

State = current context `ctx`, a call string of length ≤ k. Transitions on below-edge labels:

```
ĉ  (enter call at c):  ctx        →  ⌈c :: ctx⌉_k
č  (return at c):      ctx'       →  any ctx  with  ⌈c :: ctx⌉_k = ctx'     (nondeterministic, as kCFA)
new (allocation):      object gets heap context ⌈ctx⌉_{k-1}
```

This is kCFA's context discipline encoded as a finite automaton (one shared context), not an
independent truncation of L_C. See §6 for why this matters for phase 2.

### 2.4 Result

`O ∈ PTS(⟨v, ctx⟩)` iff there is a path `O → v` whose L_D word is accepted and which C_k
accepts ending in state `ctx` (with `O`'s heap context fixed at its allocation).

Call graph: the `dispatch[t]` edges that lie on accepted paths, with their contexts.

---

## 3. Toolchain

| Component | Choice | Notes |
|---|---|---|
| Front end | `clang++` 21 inside `svftools/svf:latest` | must match SVF's LLVM 21 |
| IR prep | `-O0 -Xclang -disable-O0-optnone -fno-discard-value-names -g`, then `opt -p=mem2reg` | locals → SSA values, smaller graph |
| PAG + call graph + CHA | SVF `wpa -ander -dump-json -dump-callgraph -dump-cha` | Andersen call graph = dispatch **candidates** (as P3Ctx uses `prePTA`) |
| Our code | Python 3 package `ldc/` | fast iteration; port hot paths to C++/SVF API only if needed |
| Solver | our own worklist CFL-R solver | SVF `cfl` cannot express typed indices + context states as needed; use it only as a cross-check for the context-free L_F part |
| Reference oracle | our own kCFA (Fig. 1 rules) over the same extracted IR | ground truth for "equal to kCFA" checks |

Known pitfall from the first SVF trial (`../cpp-pag/`): SVF's `-v-call-cha` produced **no**
virtual-call edges on our Fig. 3 port; Andersen's `callgraph_final.dot` was correct.
Use Andersen candidates.

---

## 4. Milestones

Each milestone ends with a commit and a passing test run.

### M0 — Skeleton
- `scripts/svf.sh`: docker wrapper (compile, mem2reg, run `wpa`), output to `build/<name>/`.
- `tests/cpp/`: minimal C++ ports of the paper's examples:
  - `fig3.cpp` — two call sites, receiver `A1`/`B1`, field `d.f`; plus `C` never instantiated.
  - `fig5.cpp` — **two receivers under one context** (`{B1, C1}`); the L_FC failure case.
  - `eq15.cpp` — excursion returns under a different context; the L_DC failure case.
  - `fields.cpp`, `static_calls.cpp` — no dispatch, sanity for L_F / C_k.
- `tests/expected/*.json`: hand-written expected `PTS` for the queried variables.

**Done when:** `scripts/svf.sh tests/cpp/fig3.cpp` produces SVFIR JSON + call graph.

### M1 — Extraction to a neutral graph
- `ldc/extract.py`: read SVF JSON → `Graph` (nodes, typed edges, call sites, functions,
  types). Keep SVF node IDs for traceability.
- Filter noise: `llvm.*` intrinsics, `operator new` internals (model as allocation).
- `ldc/dot.py`: dump our graph to `.dot` for inspection.

**Done when:** extracted graph of `fig3.cpp` matches a hand-drawn PAG (review by eye).

### M2 — L_F solver (k = 0, no dispatch)
- `ldc/solver.py`: worklist CFL-R for `flowsto` / `alias` with inverse edges.
- Decide representation: (a) generic CFL-R on grammar in normal form, or
  (b) specialised flowsto propagation. Start with (b) for clarity; keep a small generic
  CFL-R for cross-checks.

**Done when:** on all tests, our PTS = SVF Andersen PTS (field-sensitive, context-insensitive).

### M3 — Contexts: L_F ∩ C_k  (= "L_FC_k", the baseline)
- Add `ĉ_c` / `č_c` on call/return edges; product state `(node, ctx)`; heap context `k-1`.
- Direct calls first; indirect/virtual calls wired the L_FC way (Andersen targets → plain
  `assign(ĉ)` edges). This **is** the baseline we improve on.
- `ldc/kcfa.py`: reference kCFA (Fig. 1 rules) on the same graph.

**Done when:** L_FC_k = kCFA on `fields.cpp`, `static_calls.cpp`, `fig3.cpp` (k = 1, 2);
L_FC_k ⊋ kCFA on `fig5.cpp` (reproduces the paper's Fig. 5 loss).

### M4 — Dispatch: L_D ∩ C_k  (= L_DC_k)
- `ldc/dispatch.py`:
  1. find virtual call sites in IR (vptr load → vtable slot load → indirect call);
  2. recover dynamic type `t` of each heap object (constructor call / vptr store after
     `operator new`);
  3. candidates `m' = dispatch(c, t)` from Andersen call graph ∪ CHA;
  4. rewrite edges per §2.1 (`store[i]`, `r#c`, `dispatch[t]`, `load[i]`, `store[0]`/`load[0]`).
- Solver: index `flowsto` by type `t`; accept `dispatch[t]` only under matching `new[t]`.

**Done when:**
- `fig5.cpp`: L_DC_k = kCFA (receiver no longer crosses to the wrong target);
- `fig3.cpp`: `C::foo` absent, `v ↦ {O1}` under `[c3, c1]`;
- `eq15.cpp`: L_DC_k ⊋ kCFA (documents what L_R must fix).

### M5 — Evaluation (phase 1)
- Table per test: |PTS|, spurious receivers, call edges per context — for
  L_FC_k, L_DC_k, kCFA, SVF Andersen.
- Small real programs (a few hundred lines of C++ with a class hierarchy).
- Write `RESULTS.md`.

---

## 5. C++-specific issues (phase 1 scope decisions)

| Issue | Phase 1 decision |
|---|---|
| Multiple inheritance, `this` adjustment thunks | **out of scope**; tests use single inheritance; detect and report unsupported sites |
| Function pointers (no receiver) | handled like static calls with Andersen targets (L_FC style); no `dispatch[t]` |
| Pointer-to-member-function calls | out of scope; report |
| Object type recovery | allocation `new T(...)` = `operator new` + ctor `T::T`; take `T` from the ctor call on the returned pointer |
| Stack / global objects with virtual methods | allocation site = `alloca` / global, type from ctor |
| Arrays, unions, casts | inherit SVF's field model; casts = `assign` |
| Standard library | analyse only user code first; stub `operator new`, ignore `std::` bodies |

---

## 6. Phase 2 — L_R (sketch)

- Add boxed labels `⟦ĉ⟧` / `⟦č⟧` on the excursion edges (`r → r#c` and `r#c → this`).
- With the **single shared context** of C_k, the L_R checks become state checks:
  - DP-C1: excursion closes at the same call site → match `⟦ĉ_c⟧` with `⟦č_c⟧`;
  - DP-C2: returns "the same way" → the context state after the excursion equals the one before.
- Target theorem: L_DCR_k with one shared context = kCFA with the same k
  (heap context k−1). Validate with `ldc/kcfa.py` on all tests, then attempt a proof by
  simulation in both directions.

Naive independent k-limiting of L_C and L_R is expected to **differ** from kCFA
(heap depth k vs k−1; two unsynchronised truncations). Worth one experiment to confirm.

---

## 7. Later (not planned in detail)

- Demand-driven solver (single query from a variable; memoisation; budget) — the actual value
  over kCFA.
- Clients: devirtualisation (monomorphic call sites), cast checks, may-alias queries.
- Baselines: SVF `CFLAlias` (C++), Soot `soot.jimple.spark.ondemand.DemandCSPointsTo` (Java,
  available in `../p3ctx` image).
- Before any write-up: search papers citing P3Ctx for prior work on demand-driven L_DCR.

---

## 8. References

- P3Ctx paper: Fig. 1 (kCFA rules), Fig. 2 (L_FC PAG), Fig. 5, Fig. 6 (L_DCR PAG), Eq. 2 (L_C),
  Eq. 8 (L_D), Eq. 15 (bad excursion), Eq. 17 (L_R), Lemma 3/4, Theorem 1, Def. 2.
- Sridharan & Bodík, PLDI 2006, <https://doi.org/10.1145/1133981.1134027>.
- Sridharan, Gopan, Shan, Bodík, OOPSLA 2005, <https://doi.org/10.1145/1094811.1094817>.
- SVF: `svftools/svf` image; CFL grammars in `svf/include/CFL/grammar/`.
- Study notes: `../article.md`; talk: `../ldcr-talk/`.
