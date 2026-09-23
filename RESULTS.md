# Results — L_DC_k and L_DCR_k for C++

**Phase 2 summary (below): L_DCR_k = kCFA fact by fact on every test and program, k = 0…4,
and it removes all of L_DC's cost blow-up.** The phase-1 sections follow unchanged.

## Phase 2: L_DCR_k

`-ldc-mode=ldcr` (PLAN.md §7): L_R's DP-C1 / DP-C2 as dispatch instances (call site, caller
context) on the receiver's parameter / return fields. Measured with `ldc -ldc-eval`:

- **Equality with kCFA, fact by fact**: for every (variable, context), the same set of
  (object, heap context) — 0 differing pairs on all 6 unit tests and all 3 programs, k = 0…4.
  Negative control (L_DC in place of L_DCR): 2 (`fig8`), 10 (`eq15`), 2613 (`expr`, k = 2).
- Hence also equal Σ|pts|, virtual call edges, polymorphic sites, (function, context) pairs.
- **Cost** (ms, one run, same naive solver):

  | program | k | kCFA | L_DCR | L_DC | (fn, ctx): kCFA = L_DCR / L_DC |
  |---|---:|---:|---:|---:|---|
  | expr | 2 | 89 | 113 | 2243 | 190 / 440 |
  | expr | 3 | 42 | 60 | 22 681 | 210 / 1640 |
  | expr | 4 | 45 | 63 | 137 355 | 240 / 4909 |
  | shapes | 4 | 60 | 68 | 367 | 332 / 576 |

  L_DCR costs 10–50 % more than kCFA here (extra tagged facts in `this`); L_DC's spurious
  objects create spurious contexts and grow with k — finding 5 below, now confirmed: precision
  pays for itself.
- **What this does and does not show.** The solver is inclusion-based, and in that form the
  equality is close to by construction (proof sketch in PLAN.md §7). It shows that the
  regularised L_DCR_k with *one shared context* is the right target — L_DC_k is not, on real
  patterns (visitors). It does not yet show the CFL side (path ↔ derivation) or give a
  demand-driven solver; that is the next step.

# Phase 1

Evaluation of M5 (PLAN.md §4), before phase 2. Three analyses run on the same LDGraph and the same memory
model, for k = 0, 1, 2:

- **kCFA** — the oracle (paper Fig. 1): per-receiver dispatch, call-string contexts, heap
  context k−1.
- **L_DC** — L_D ∩ C_k on the paper's Fig. 6 edges (phase 1 of this project).
- **L_FC** — L_F ∩ C_k with virtual calls wired from SVF Andersen's call graph (the
  baseline: SVF `CFLAlias`, Soot `DemandCSPointsTo`, and SVF Andersen itself behave this way).

SVF Andersen (context-insensitive, all functions) is shown for reference.

Reproduce:

```
cmake -S . -B build && cmake --build build
ctest --test-dir build                  # includes <program>.eval.k<k>: soundness vs kCFA
cmake --build build --target eval       # writes build/eval/results.md
```

## Programs

Written for this evaluation (`eval/cpp/`), in the phase-1 scope: single inheritance, no STL.

| program | lines | what it stresses | virtual sites | CHA targets | Andersen targets |
|---|---:|---|---:|---:|---:|
| `events` | 159 | chain of responsibility, commands in an undo stack (array), getters/setters | 13 | 39 | 25 |
| `expr` | 211 | AST + visitors (double dispatch), environments, builder functions called many times | 41 | 214 | 120 |
| `shapes` | 152 | decorators, factory, `void*` list container, observers | 14 | 57 | 53 |

## Metrics

- **Σ|pts|** — sum over all variables of the context-insensitive points-to set size (objects
  projected to their allocation site). Lower is more precise.
- **extra** — objects over kCFA, summed over variables. **missing** — objects kCFA has and the
  analysis lacks (a soundness failure w.r.t. the oracle). Missing is **0 everywhere**.
- **vcall edges** — (virtual call site, callee) pairs; **poly** — virtual sites with ≥ 2
  callees (not devirtualisable); **(fn, ctx)** — analysed (function, context) pairs.
- **ms** — solve time; one run, naive round-robin solver: only the ratios mean anything.

## Tables

### events

| k | analysis | Σ\|pts\| | extra | vcall edges | poly | (fn, ctx) | ms |
|---|---|---:|---:|---:|---:|---:|---:|
| 0 | kCFA | 341 | 0 | 23 | 7 | 40 | 5.1 |
| 0 | L_DC | 341 | 0 | 23 | 7 | 40 | 5.8 |
| 0 | L_FC | 493 | 152 | 25 | 7 | 40 | 5.5 |
| 1 | kCFA | 325 | 0 | 21 | 6 | 68 | 11.3 |
| 1 | L_DC | 325 | 0 | 21 | 6 | 68 | 12.8 |
| 1 | L_FC | 467 | 142 | 25 | 7 | 72 | 11.5 |
| 2 | kCFA | 325 | 0 | 21 | 6 | 90 | 13.3 |
| 2 | L_DC | 325 | 0 | 21 | 6 | 90 | 14.1 |
| 2 | L_FC | 467 | 142 | 25 | 7 | 119 | 19.4 |
| – | SVF Andersen | 452 | 111–127 | 25 | 7 | – | – |

### expr

| k | analysis | Σ\|pts\| | extra | vcall edges | poly | (fn, ctx) | ms |
|---|---|---:|---:|---:|---:|---:|---:|
| 0 | kCFA | 1133 | 0 | 80 | 25 | 62 | 24.1 |
| 0 | L_DC | 2620 | 1487 | 128 | 32 | 62 | 52.4 |
| 0 | L_FC | 3436 | 2303 | 120 | 36 | 63 | 51.3 |
| 1 | kCFA | 643 | 0 | 57 | 12 | 129 | 27.3 |
| 1 | L_DC | 2530 | 1887 | 125 | 31 | 197 | 160.8 |
| 1 | L_FC | 3086 | 2443 | 120 | 36 | 193 | 130.3 |
| 2 | kCFA | 597 | 0 | 57 | 12 | 190 | 33.0 |
| 2 | L_DC | 2388 | 1791 | 108 | 26 | 440 | 979.4 |
| 2 | L_FC | 3003 | 2406 | 120 | 36 | 499 | 527.4 |
| – | SVF Andersen | 2648 | 1515–2051 | 120 | 36 | – | – |

### shapes

| k | analysis | Σ\|pts\| | extra | vcall edges | poly | (fn, ctx) | ms |
|---|---|---:|---:|---:|---:|---:|---:|
| 0 | kCFA | 582 | 0 | 53 | 14 | 40 | 9.6 |
| 0 | L_DC | 914 | 332 | 53 | 14 | 40 | 14.9 |
| 0 | L_FC | 1039 | 457 | 53 | 14 | 40 | 14.2 |
| 1 | kCFA | 563 | 0 | 53 | 14 | 109 | 19.5 |
| 1 | L_DC | 876 | 313 | 53 | 14 | 109 | 30.3 |
| 1 | L_FC | 1039 | 476 | 53 | 14 | 109 | 38.6 |
| 2 | kCFA | 541 | 0 | 53 | 14 | 205 | 40.2 |
| 2 | L_DC | 681 | 140 | 53 | 14 | 221 | 63.0 |
| 2 | L_FC | 869 | 328 | 53 | 14 | 229 | 91.1 |
| – | SVF Andersen | 1001 | 419–460 | 53 | 14 | – | – |

## Findings

1. **Soundness.** On every program and every k, L_DC ⊇ kCFA and L_FC ⊇ kCFA (0 missing
   objects). This is a CTest gate (`<program>.eval.k<k>`).

2. **L_DC is always at least as precise as L_FC in Σ|pts|**, and it removes 23–100 % of L_FC's
   extra objects: `events` 100 % (L_DC = kCFA at every k), `shapes` 27–57 %, `expr` 23–35 %.

3. **The remaining loss is the paper's DP-C1 (Fig. 8), and the visitor pattern triggers it at
   scale.** In `expr`, every `v->visitX(this)` stores the AST node in the parameter field `p1`
   of the *same* visitor object, and every `visitX` loads it back. So in L_DC,
   `Evaluator::visitMul::e` points to all 10 AST allocation sites; kCFA gives the 2 `Mul`
   sites; L_FC gives 18 (k = 2). The extra objects then reach the receivers of the next
   `accept` calls: L_DC has *more* virtual call edges than L_FC on `expr` at k = 0, 1
   (128/125 vs 120), and fewer only at k = 2 (108). This is exactly what L_R's `siteRecovered`
   (DP-C1) is for — phase 2 has a concrete target.

   In `events` the same merge happens (`c->run(doc)` and `c->undo(doc)` share `p1` of a
   command object) but it is harmless: both pass the same `doc`.

4. **In `shapes` the loss sits in the decorators' forwarding methods** (`anchor`, `area`,
   `moved` of `Scaled` / `Labeled`, and `main`): `inner->anchor()` etc. are called on the same
   objects from several call sites and caller contexts, so the `ret` / `p_i` fields merge
   results — DP-C1 and DP-C2 together; this evaluation does not separate the two. L_DC's gap
   to kCFA shrinks as k grows (332 → 313 → 140): deeper heap contexts split the receiver
   objects, which is what limits DP-C2.

5. **Imprecision costs time.** On `expr` at k = 2, kCFA analyses 190 (function, context) pairs
   in 33 ms; L_DC 440 in 979 ms; L_FC 499 in 527 ms. Spurious objects create spurious contexts —
   the usual k-CFA effect. We therefore expect a precise L_DCR to be *cheaper* than L_DC here.

6. **Call graph.** kCFA's call graph is the most precise everywhere (`expr`: 12 polymorphic
   sites at k ≥ 1 vs 36 for Andersen). L_DC reaches it on `events` and `shapes`, not on `expr`
   (finding 3).

## C++ / SVF findings made during M5 (details in PLAN.md §6)

- **The declared type of a virtual call** (`t <: DeclTypeOf(r)` in [C-VCall]) is needed in C++:
  without it, slot-based CHA dispatched `root->handle(e)` to `Copy::run` (same vtable slot, an
  unrelated hierarchy). SVF's `getFunNameOfVirtualCall()` is empty without SVF's preprocessing
  pass; clang's `-fwhole-program-vtables` type tests give the static class instead.
- **Memory model.** Two fixes applied to all analyses: array elements keep the array's field
  (SVF's model) instead of `*`; interior pointers (`&bus.history` passed as `this`) are field
  objects ⟨O, off⟩ instead of copies of the base. Before these fixes and the declared-type
  filter, kCFA had spurious call edges and objects, so L_FC looked unsound against it.
- **L_FC at k = 0 vs SVF Andersen** is exact on the unit tests (M2) but differs on 10–61
  variables of these programs. The differences seen are memory-model details (loads through
  `char*` in `expr`'s `same()`, `const char*` constructor arguments in `shapes`); they were not
  analysed further. All three of our analyses share one memory model, so their comparison is
  unaffected; the Andersen row is for orientation only.

## Threats to validity

- The programs are small and written for this study (not an external benchmark). They were
  not changed after the first run; only the analysis was fixed (the issues listed above).
- Phase-1 scope: single inheritance, no STL, no function pointers of note, no exceptions paths.
- kCFA is our own implementation on LDGraph; it is validated against hand-written expectations
  on the unit tests (`tests/`), not against an independent kCFA tool.
- Times: one run each, unoptimised solver, inside Docker.

## Next

- Phase 2: L_R (PLAN.md §7). Targets: `fig8` / `expr` (DP-C1) and `eq15` / `shapes` (DP-C2);
  success = L_DCR_k = kCFA on all of them.
- Larger programs need an STL story (models or analysing `std::` bodies) and multiple
  inheritance (thunks, secondary vtables).
