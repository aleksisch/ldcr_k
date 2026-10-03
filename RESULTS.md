# Results — L_DC_k and L_DCR_k for C++

**Summary.**
- **L_DCR_k = kCFA fact by fact** — for every (variable, context) the same set of
  (object, heap context) — on all 7 unit tests, the 3 small programs (k = 0…4) and the real
  program tinyxml2 (8 k lines; k = 0, 1).
- **L_DC_k is sound w.r.t. kCFA but not precise**, and on tinyxml2 it is *less* precise than
  the L_FC baseline. Its loss is the paper's DP-C1 / DP-C2 (Eq. 13, 15), dominated by visitor
  and pool patterns.
- **The L_FC baseline built on SVF Andersen is unsound on real C++**: SVF's call graph lacks
  29 virtual call edges on tinyxml2 (pool `Free` / `Alloc`, a virtual destructor).
- **k = 2 on tinyxml2 does not finish** in 25 min for any mode (37–80 M facts and growing).

Four analyses run on the same LDGraph and the same memory model:

- **kCFA** — the oracle (paper Fig. 1): per-receiver dispatch, call-string contexts, heap
  context k−1.
- **L_DCR** — L_D ∩ C_k ∩ L_R (phase 2, PLAN.md §7).
- **L_DC** — L_D ∩ C_k on the paper's Fig. 6 edges (phase 1).
- **L_FC** — L_F ∩ C_k with virtual calls wired from SVF Andersen's call graph (the baseline:
  SVF `CFLAlias`, Soot `DemandCSPointsTo`, SVF Andersen itself behave this way).

Reproduce:

```
cmake -S . -B build && cmake --build build
ctest --test-dir build                  # unit tests, soundness + L_DCR = kCFA gates
./build/ldc -ldc-k=<k> -ldc-mode=<m> build/tests/<program>.ll   # one table row ("Solver", "Result")
```

tinyxml2 is not built by CMake: compile `tinyxml2.cpp` and `xmltest.cpp` with the flags of
`cmake/LdcIR.cmake`, link them with `llvm-link`, run `opt -p=mem2reg`, then
run `ldc` on `tinyxml2.ll` as above.

## Metrics

- **Σ|pts|** — sum over all variables of the context-insensitive points-to set size (objects
  projected to their allocation site). Lower is more precise.
- **extra** — objects over kCFA, summed over variables: Σ|pts| minus kCFA's. **missing** —
  kCFA facts the analysis lacks (a soundness failure w.r.t. the oracle), from comparing the
  `-ldc-facts` files.
- **vcall edges** — (virtual call site, callee) pairs; **poly** — virtual sites with ≥ 2
  callees; **(fn, ctx)** — analysed (function, context) pairs.
- **ms / s** — solve time, one run, `RelWithDebInfo`, inside Docker.

## Real program: tinyxml2

`leethomason/tinyxml2` at `8224e42`: `tinyxml2.cpp` + `xmltest.cpp` (its own test driver),
8278 lines, linked to one module. LDGraph: 7792 nodes (904 objects), 11 579 edges,
2708 call sites, 107 virtual call sites (592 CHA targets), 6 construction sites.

| k | analysis | Σ\|pts\| | extra | missing | vcall edges | poly | (fn, ctx) | s |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 0 | kCFA | 323 092 | 0 | 0 | 291 | 56 | 405 | 5.5 |
| 0 | L_DCR | 323 092 | 0 | 0 | 291 | 56 | 405 | 8.3 |
| 0 | L_DC | 553 225 | 230 133 | 0 | 469 | 65 | 436 | 11.1 |
| 0 | L_FC | 449 775 | 126 683 | 0 | 434 | 84 | 437 | 16.4 |
| 1 | kCFA | 321 080 | 0 | 0 | 291 | 56 | 2784 | 24.2 |
| 1 | L_DCR | 321 080 | 0 | 0 | 291 | 56 | 2784 | 52.4 |
| 1 | L_DC | 553 225 | 232 145 | 0 | 469 | 65 | 3007 | 52.3 |
| 1 | L_FC | 449 495 | 128 415 | 0 | 434 | 84 | 2980 | 53.0 |

- **L_DCR = kCFA** per (variable, context), k = 0 and 1.
- **L_DC is worse than L_FC** here (553 k vs 450 k; 469 vs 434 virtual call edges), unlike on
  every small program. The extra objects concentrate in `main` (the test driver calls many
  `XMLDocument` / `XMLPrinter` methods on the same objects), `XMLPrinter` and the parser —
  the receiver fields `p_i` / `ret` of long-lived objects merge all their calls (DP-C1).
- **SVF Andersen's call graph lacks 29 virtual call edges** that kCFA finds: `MemPoolT<N>::Free`
  / `Alloc` called through the `_memPool` pointer stored in each node, and
  `~XMLAttribute()`. L_FC, wired from it, lacks the same edges.
- **Context sensitivity barely helps** at k = 1 (323 k → 321 k): the smear comes from the
  memory model — pool blocks that SVF makes field-insensitive, and all nodes built in them.
- **k = 2 does not finish** in 25 min for any mode: at the timeout kCFA had 11.5 k
  (function, context) pairs and 61 M facts, L_DCR 81 k objects and 460 k heap cells (its
  per-instance cells), L_DC and L_FC 62–80 M facts. Heap context depth 1 multiplies the
  smeared pool contents per context.
- Untyped receivers (dropped by kCFA / L_DCR / L_DC): only heap buffers (`new char[]`) and
  vtables reaching receivers through the smear — no class objects.

## Small programs

Written for this study (`tests/cpp/`): `events` (159 lines; handler chain, undo stack),
`expr` (211; AST + visitors), `shapes` (152; decorators, factory, `void*` list, observers).
L_DCR = kCFA per (variable, context) at every k.

### events

| k | analysis | Σ\|pts\| | extra | missing | vcall edges | poly | (fn, ctx) | ms |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 0 | kCFA | 341 | 0 | 0 | 23 | 7 | 40 | 0.4 |
| 0 | L_DCR | 341 | 0 | 0 | 23 | 7 | 40 | 0.4 |
| 0 | L_DC | 341 | 0 | 0 | 23 | 7 | 40 | 0.3 |
| 0 | L_FC | 515 | 174 | 0 | 25 | 7 | 40 | 0.3 |
| 1 | kCFA | 325 | 0 | 0 | 21 | 6 | 68 | 0.6 |
| 1 | L_DCR | 325 | 0 | 0 | 21 | 6 | 68 | 0.6 |
| 1 | L_DC | 325 | 0 | 0 | 21 | 6 | 68 | 0.5 |
| 1 | L_FC | 495 | 170 | 0 | 25 | 7 | 72 | 0.6 |
| 2 | kCFA | 325 | 0 | 0 | 21 | 6 | 90 | 1.0 |
| 2 | L_DCR | 325 | 0 | 0 | 21 | 6 | 90 | 0.9 |
| 2 | L_DC | 325 | 0 | 0 | 21 | 6 | 90 | 0.8 |
| 2 | L_FC | 495 | 170 | 0 | 25 | 7 | 119 | 1.2 |

### expr

| k | analysis | Σ\|pts\| | extra | missing | vcall edges | poly | (fn, ctx) | ms |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 0 | kCFA | 1133 | 0 | 0 | 80 | 25 | 62 | 1.0 |
| 0 | L_DCR | 1133 | 0 | 0 | 80 | 25 | 62 | 1.2 |
| 0 | L_DC | 2650 | 1517 | 0 | 128 | 32 | 62 | 1.4 |
| 0 | L_FC | 3976 | 2843 | 0 | 120 | 36 | 63 | 1.2 |
| 1 | kCFA | 643 | 0 | 0 | 57 | 12 | 129 | 1.5 |
| 1 | L_DCR | 643 | 0 | 0 | 57 | 12 | 129 | 1.8 |
| 1 | L_DC | 2560 | 1917 | 0 | 125 | 31 | 197 | 3.5 |
| 1 | L_FC | 3570 | 2927 | 0 | 120 | 36 | 193 | 2.5 |
| 2 | kCFA | 597 | 0 | 0 | 57 | 12 | 190 | 2.1 |
| 2 | L_DCR | 597 | 0 | 0 | 57 | 12 | 190 | 2.3 |
| 2 | L_DC | 2418 | 1821 | 0 | 108 | 26 | 440 | 18.6 |
| 2 | L_FC | 3487 | 2890 | 0 | 120 | 36 | 499 | 10.2 |

### shapes

| k | analysis | Σ\|pts\| | extra | missing | vcall edges | poly | (fn, ctx) | ms |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 0 | kCFA | 582 | 0 | 0 | 53 | 14 | 40 | 0.5 |
| 0 | L_DCR | 582 | 0 | 0 | 53 | 14 | 40 | 0.5 |
| 0 | L_DC | 914 | 332 | 0 | 53 | 14 | 40 | 0.5 |
| 0 | L_FC | 1363 | 781 | 0 | 53 | 14 | 40 | 0.4 |
| 1 | kCFA | 563 | 0 | 0 | 53 | 14 | 109 | 1.0 |
| 1 | L_DCR | 563 | 0 | 0 | 53 | 14 | 109 | 1.2 |
| 1 | L_DC | 876 | 313 | 0 | 53 | 14 | 109 | 1.1 |
| 1 | L_FC | 1363 | 800 | 0 | 53 | 14 | 109 | 0.9 |
| 2 | kCFA | 541 | 0 | 0 | 53 | 14 | 205 | 1.6 |
| 2 | L_DCR | 541 | 0 | 0 | 53 | 14 | 205 | 1.8 |
| 2 | L_DC | 681 | 140 | 0 | 53 | 14 | 221 | 2.4 |
| 2 | L_FC | 869 | 328 | 0 | 53 | 14 | 229 | 1.9 |

## Findings

1. **Soundness.** On every program and every k, L_DCR, L_DC and L_FC ⊇ kCFA (0 missing
   objects). CTest gate (`<program>.<ldc|lfc>-sound.k<k>`).
2. **L_DCR_k = kCFA fact by fact** everywhere it finishes (gate as well:
   `<program>.ldcr-same.k<k>`). Negative control: comparing L_DC instead gives 2 / 10 / 20 619
   differing facts on `fig8` / `eq15` / `expr` (k = 2).
3. **L_DC vs L_FC is program-dependent.** On the small programs L_DC removes part of L_FC's
   extra objects (`events` 100 %, `shapes` 57–61 %, `expr` 35–47 %); on tinyxml2 it adds
   more than L_FC has (+82 %). Its loss is DP-C1 / DP-C2: the receiver's `p_i` / `ret` fields
   are shared by all call sites and caller contexts. The visitor in `expr`
   (`Evaluator::visitMul::e`: 10 objects in L_DC, 2 in kCFA, k = 2) and long-lived objects
   with many methods in tinyxml2 trigger it at scale.
4. **Imprecision costs time.** `expr`: kCFA and L_DCR analyse 190 / 210 / 240
   (function, context) pairs at k = 2 / 3 / 4 in 1.4–2 ms; L_DC 440 / 1640 / 4909 in
   12 / 70 / 937 ms. Spurious objects create spurious contexts. L_DCR costs 10–30 % more
   than kCFA on the small programs and 1.5–2.2× on tinyxml2 (tagged receivers,
   per-instance cells).
5. **The SVF-based baseline is unsound on real C++** (29 missing virtual call edges on
   tinyxml2; also `placement.cpp`, where SVF resolves neither virtual call).

## Phase 2: L_DCR_k

`-ldc-mode=ldcr` (PLAN.md §7): L_R's DP-C1 / DP-C2 as dispatch instances (call site, caller
context) on the receiver's parameter / return fields. In inclusion form the equality with
kCFA is close to by construction (proof sketch in PLAN.md §7); it shows that the
regularised L_DCR_k with *one shared context* is the right target, and it does not yet show
the CFL side (path ↔ derivation) or give a demand-driven solver.

| program | k | (fn, ctx): kCFA = L_DCR / L_DC | ms: kCFA / L_DCR / L_DC |
|---|---:|---|---|
| expr | 2 | 190 / 440 | 1.4 / 1.6 / 12.1 |
| expr | 3 | 210 / 1640 | 1.8 / 2.0 / 69.5 |
| expr | 4 | 240 / 4909 | 1.5 / 1.9 / 937 |
| shapes | 4 | 332 / 576 | 2.2 / 2.4 / 8.2 |

## P3Ctx and L_DCR_k as CFL-reachability

**P3Ctx** (`-ldc-p3ctx`, He et al. ECOOP '24, Fig. 10 / Eq. 28): nodes that cannot matter
are analysed in context [] only. tinyxml2, one run each, load 3–5:

| k | mode | full | P3Ctx | nodes kept | facts (contexts dropped) |
|---:|---|---:|---:|---:|---|
| 1 | kcfa | 35.1 s | 19.8 s | 1 980 / 7 685 | = full kCFA |
| 1 | ldcr | 81.4 s | 54.2 s | 2 050 / 7 792 | = full kCFA |
| 2 | kcfa | — | 292.7 s | 1 980 / 7 685 | |
| 2 | ldcr | — | 948.0 s | 2 050 / 7 792 | = P3Ctx kcfa (238 636) |

**CFL-reachability** (`cfl/`): `ldc -ldc-export` writes LDGraph and P3Ctx's choice;
`cfl/ldcr.py` builds the product G × R_k, writes L_D as a CNF grammar and solves it with
the matrix-based all-pairs solver of Muravev & Grigorev (SOAP '25, `third_party/CFPQ_PyAlgo`,
GraphBLAS on CPU). The (function, context) instances are not taken from `ldc`: an outer loop
adds those that proven dispatches reach and solves again. `cfl/check.sh` compares with the
worklist fact by fact, contexts included: equal on every unit test (k = 0–2, with and
without P3Ctx) and on tinyxml2:

| k | worklist ldcr + P3Ctx | CFL: solver time (rounds) | product | pairs | facts |
|---:|---:|---:|---|---:|---|
| 0 | 17.5 s | 521 s (4) | 21.8k nodes, 0.89M edges | 2.7M | 283 091, equal |
| 1 | 52.3 s | 997 s (4) | 35.4k nodes, 0.95M edges | 7.4M | 743 018, equal |

Each round is solved from scratch; the last one alone is 137 s (k = 0) / 282 s (k = 1).

**k = 0 vs k = 1, all modes, worklist, no P3Ctx** (tinyxml2): L_DC_k is less precise than
kCFA even at k = 0 (arguments of all calls on one receiver meet in its fields) and is
slower because of it; L_DCR_k has kCFA's facts, and its cost over kCFA grows with k (one
cell and one tagged `this` per dispatch instance (c, C)).

| k | kcfa | ldcr | ldc | lfc | Σ\|pts\| kcfa = ldcr / ldc / lfc |
|---:|---:|---:|---:|---:|---|
| 0 | 10.6 s | 12.3 s | 23.9 s | 26.0 s | 323 092 / 553 225 / 449 775 |
| 1 | 35.0 s | 85.4 s | 94.9 s | 73.1 s | 321 080 / 553 225 / 449 495 |

## C++ / SVF findings (details in PLAN.md §6)

Each was found by the evaluation and fixed for **all** analyses:

- **Declared type of a virtual call** (`t <: DeclTypeOf(r)`): needed, or slot-based CHA
  dispatches into unrelated hierarchies. SVF's `getFunNameOfVirtualCall()` is empty without
  its preprocessing pass; clang's `-fwhole-program-vtables` type tests give the class.
- **Construction sites and member subobjects.** Placement new (`new (pool.Alloc()) T`) gives
  each construction site its own object of class T; members constructed in a constructor
  (`T::T(this + off)`) give ⟨O, off⟩ a dynamic type, also for class templates. Without this,
  every tinyxml2 node and its pools were untyped and silently dropped at virtual calls.
- **Memory model:** array elements keep the array's field; escaping geps (stored, passed,
  returned, phi/select) are interior pointers ⟨O, off⟩; offsets are capped by SVF's field
  limit, and objects SVF makes field-insensitive are field-insensitive here too (a receiver
  at offset `*` may then have the type of the object or of any polymorphic member).
- **L_FC at k = 0 vs SVF Andersen** is exact on the unit tests (M2) except `placement`
  (SVF misses the calls); on the programs it differs in memory-model details. All four of
  our analyses share one memory model, so their comparison is unaffected.

## Threats to validity

- One real program; the small ones were written for this study. None was changed after its
  first run; only the analysis was fixed (the issues above).
- Phase-1 scope: single inheritance, no STL, no exceptions paths.
- kCFA is our own implementation on LDGraph, validated against hand-written expectations on
  the unit tests, not against an independent kCFA tool.
- Times: one run each, inside Docker.

## Next

- CFL solver speed: warm start between rounds, reverse relations as transposes, a GPU
  SpGEMM backend; the product and the loop in C++.
- A demand-driven solver (PLAN.md §8).
- Larger programs need STL and multiple inheritance.
