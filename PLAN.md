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

## 2. Two graphs: SVFIR and LDGraph

**SVFIR** is SVF's PAG, available in memory through the SVF C++ API after `SVFIRBuilder`.
Nodes: value variables and object variables. Edges: `Addr`, `Copy`, `Load`, `Store`, `Gep`,
`CallPE` / `RetPE` (with call site), and others. It is what SVF's Andersen and `CFLAlias` run on.

SVFIR does **not** contain what L_D needs:
- the dynamic type on allocation edges (`new[t]`);
- argument passing routed through the receiver (`store[i]` / `load[i]`);
- per-call-site receiver copies `r#c` and `dispatch[t]` edges.

**LDGraph** is our own graph, built by **transforming SVFIR** per paper Fig. 6. The solver and
the kCFA oracle both run on LDGraph. SVF node IDs are kept on every LDGraph node for
traceability. (SVF's `-dump-json` is not used; optional for debugging only.)

### 2.1 LDGraph labels

Each edge also has an inverse `ℓ‾`.

| Label (above edge) | Below-edge | Built from |
|---|---|---|
| `new[t]` | — | `Addr` edge; `t` recovered from the constructor call (§5) |
| `assign` | — | `Copy` (copy, cast, phi) |
| `store[f]`, `load[f]` | — | `Store` / `Load` with the `Gep` offset path as `f` |
| `assign` | `ĉ_c` / `č_c` | `CallPE` / `RetPE` at static calls and receiver-less indirect calls |
| `store[i]` | `ĉ_c` | virtual call `c`: argument `a_i` → receiver `r` (synthetic field `i`) |
| `load[i]` | — | callee: `this^m` → parameter `p_i` |
| `store[0]` / `load[0]` | — / `č_c` | callee `ret^m` → `this^m`; caller `r` → lhs |
| `assign` | — | `r` → `r#c` |
| `dispatch[t]` | `ĉ_c` | `r#c` → `this^m'`, for each candidate `t`, `m' = dispatch(c, t)` |

These are the rules [C-New], [C-Param], [C-Ret], [C-VCall] of paper Fig. 6.
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
Implementation: `flowsto` facts carry the type `t` of their origin object; `dispatch[t]` is
traversable only when it matches.

### 2.3 Regular context automaton C_k

State = current context `ctx`, a call string of length ≤ k. Transitions on below-edge labels:

```
ĉ  (enter call at c):  ctx        →  ⌈c :: ctx⌉_k
č  (return at c):      ctx'       →  any ctx  with  ⌈c :: ctx⌉_k = ctx'     (nondeterministic, as kCFA)
new (allocation):      object gets heap context ⌈ctx⌉_{k-1}
```

This is kCFA's context discipline encoded as a finite automaton (one shared context), not an
independent truncation of L_C. See §7 for why this matters for phase 2.

### 2.4 Result

`O ∈ PTS(⟨v, ctx⟩)` iff there is a path `O → v` in LDGraph whose L_D word is accepted and which
C_k accepts ending in state `ctx` (with `O`'s heap context fixed at its allocation).

Call graph: the `dispatch[t]` edges that lie on accepted paths, with their contexts.

---

## 3. Toolchain and build

| Component | Choice |
|---|---|
| Language | C++17 |
| Build | **CMake** + CTest |
| SVF + LLVM | `svftools/svf:latest` image (SVF 3.4, LLVM 21.1.0); `find_package(SVF CONFIG)` via `SVF_DIR=/home/SVF-tools/SVF/Release-build/lib/cmake/SVF`, `LLVM_DIR` from the same image |
| Test front end | `clang++` 21 from the image; `-O0 -Xclang -disable-O0-optnone -fno-discard-value-names -g`, then `opt -p=mem2reg` |
| Dispatch candidates | SVF Andersen call graph (as P3Ctx uses `prePTA`) ∪ SVF CHA |
| Reference oracle | our own kCFA (paper Fig. 1 rules) on LDGraph |

Development happens inside the image: the repo is mounted into a container based on
`svftools/svf:latest` (a `Dockerfile` in the repo pins it). No shell wrapper scripts:

```
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

CMake drives test compilation: for each `tests/cpp/*.cpp`, an `add_custom_command` produces the
`.ll` (clang++ → opt mem2reg), and an `add_test` runs `ldc` on it and compares the result with
`tests/expected/<name>.json`.

Known pitfalls with the `svftools/svf` image:
- SVF's `-v-call-cha` produced **no** virtual-call edges on our Fig. 3 port (`../cpp-pag/`);
  Andersen's final call graph was correct. Use Andersen candidates.
- `SVF::SvfLLVM` exports a non-existent include dir (`Release-build/include/SVF`); CMake refuses
  to generate. The `Dockerfile` creates it.
- `LLVMModuleSet::preProcessBCs()` (used by SVF's `svf-ex`) writes `<name>.pre.bc`, fails with
  "Bad file descriptor", and segfaults — `svf-ex` itself crashes. `ldc` follows `wpa` and skips it.

### 3.1 Repo layout (target)

```
CMakeLists.txt
Dockerfile                 FROM svftools/svf:latest
include/ldc/               LDGraph.h  Builder.h  Dispatch.h  Solver.h  KCFA.h  Context.h
src/                       one .cpp per header + main.cpp (the `ldc` tool)
tests/cpp/                 fig3.cpp  fig5.cpp  eq15.cpp  fields.cpp  static_calls.cpp
tests/expected/            <name>.json  hand-written expected PTS for queried variables
tests/CMakeLists.txt
```

`ldc` CLI: options go through SVF's parser (`-ldc-*`), so they coexist with SVF's own:
`ldc [-ldc-dot=<out>] <file.ll>`; to come: `-ldc-k=<n>`, `-ldc-mode={lfc,ldc,kcfa}`,
`-ldc-query=<var>`, `-ldc-json=<out>`.

---

## 4. Milestones

Each milestone ends with a commit and a passing `ctest`.

### M0 — Skeleton
- `CMakeLists.txt`, `Dockerfile`; an `ldc` binary that loads a `.ll` through SVF, runs
  Andersen, and prints SVFIR statistics + the resolved call graph.
- Test programs in `tests/cpp/` (minimal C++ ports of the paper's examples):
  - `fig3.cpp` — two call sites, receivers `A1`/`B1`, field `d.f`; `C` exists but never reaches `x`.
  - `fig5.cpp` — **two receivers under one context** (`{B1, C1}`); the L_FC failure case.
  - `eq15.cpp` — excursion returns under a different context; the L_DC failure case.
  - `fields.cpp`, `static_calls.cpp` — no dispatch; sanity for fields and contexts.
- `tests/expected/*.json`: hand-written expected `PTS` for the queried variables.

**Done when:** `cmake --build` + `ctest` compile all tests to `.ll` and `ldc` runs on each.

### M1 — SVFIR → LDGraph
- `Builder`: walk SVFIR, emit LDGraph nodes/edges for `new`, `assign`, `store[f]`, `load[f]`,
  and call/return edges with `ĉ_c` / `č_c` (no dispatch rewriting yet: virtual calls wired
  as plain `assign(ĉ)` to Andersen targets).
- Filter noise: `llvm.*` intrinsics; model `operator new` as an allocation site.
- `--dot` dump of LDGraph.

**Done when:** LDGraph of `fig3.cpp` matches a hand-drawn PAG (reviewed by eye).

### M2 — L_F solver (k = 0, no dispatch)
- `Solver`: worklist CFL-R for `flowsto` / `alias` with inverse edges.

**Done when:** on all tests, our PTS = SVF Andersen PTS (field-sensitive, context-insensitive).

### M3 — Baseline L_FC_k and the kCFA oracle
Three purposes:
1. **Baseline** to measure against: L_F ∩ C_k with virtual calls wired the L_FC way
   (Andersen targets → plain `assign(ĉ)` edges). This is what SVF `CFLAlias` and Soot
   `DemandCSPointsTo` do.
2. **Oracle**: `KCFA` implements the paper's Fig. 1 rules on LDGraph — the ground truth.
3. **Reproduce the defect** before fixing it.

**Done when:**
- `fields.cpp`, `static_calls.cpp`, `fig3.cpp` (k = 1, 2): L_FC_k = kCFA
  (validates C_k and heap context k−1);
- `fig5.cpp`: L_FC_k ⊋ kCFA — the Fig. 5 loss is reproduced.

### M4 — Dispatch: L_D ∩ C_k  (= L_DC_k)
- `Dispatch`:
  1. find virtual call sites (vptr load → vtable slot load → indirect call);
  2. recover the dynamic type `t` of each heap object (§5);
  3. candidates `m' = dispatch(c, t)` from Andersen call graph ∪ CHA;
  4. rewrite LDGraph edges at those sites per §2.1.
- `Solver`: `flowsto` carries the origin type; `dispatch[t]` only under matching `new[t]`.

**Done when:**
- `fig5.cpp`: L_DC_k = kCFA (the receiver no longer crosses to the wrong target);
- `fig3.cpp`: `C::foo` absent; `v ↦ {O1}` under `[c3, c1]`;
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
| Object type recovery | `new T(...)` = `operator new` + ctor `T::T`; take `T` from the ctor call on the returned pointer |
| Stack / global objects with virtual methods | allocation site = `alloca` / global, type from ctor |
| Arrays, unions, casts | inherit SVF's field model; casts = `assign` |
| Standard library | analyse only user code first; model `operator new`, ignore `std::` bodies |

---

## 6. Findings about SVFIR (settled in M1)

- **Virtual call sites** are recognised by SVF on LLVM 21: `CallICFGNode::isVirtualCall()`,
  `getVtablePtr()`, `getFunIdxInVtable()`, `getActualParms()`. In IR the dispatch is visible as
  `r --load[0]--> vtable --load[0]--> fnptr`.
- **Calls:** `CallPE` is a multi-operand statement (formal ← one actual per call site),
  `RetPE` is formal return → actual return. Both exist **only for direct calls**. Indirect and
  virtual call sites have no call/return statements in SVFIR (Andersen connects them only in its
  own constraint graph), so `Builder` wires them from Andersen's call graph.
- **Fields:** SVFIR models `p->f = v` as `q = gep p, f; *q = v`. `Builder` folds the gep into the
  load/store: `v --store[f]--> p`, with `f` = flattened constant offset; a direct `*p` is field 0;
  a variable offset is field `*`. A gep result used as a value ("escaping gep") is counted and
  reported; in the tests these are the vptr stores in constructors.
- **Noise** kept in LDGraph for now: vtable / typeinfo globals and function objects. The vtable
  stores in constructors are the input for type recovery in M4.

---

## 7. Phase 2 — L_R (sketch)

- Add boxed labels `⟦ĉ⟧` / `⟦č⟧` on the excursion edges (`r → r#c` and `r#c → this`).
- With the **single shared context** of C_k, the L_R checks become state checks:
  - DP-C1: excursion closes at the same call site → match `⟦ĉ_c⟧` with `⟦č_c⟧`;
  - DP-C2: returns "the same way" → the context state after the excursion equals the one before.
- Target theorem: L_DCR_k with one shared context = kCFA with the same k
  (heap context k−1). Validate with `KCFA` on all tests, then attempt a proof by
  simulation in both directions.

Naive independent k-limiting of L_C and L_R is expected to **differ** from kCFA
(heap depth k vs k−1; two unsynchronised truncations). Worth one experiment to confirm.

---

## 8. Later (not planned in detail)

- Demand-driven solver (single query from a variable; memoisation; budget) — the actual value
  over kCFA.
- Clients: devirtualisation (monomorphic call sites), cast checks, may-alias queries.
- Baselines: SVF `CFLAlias` (C++), Soot `soot.jimple.spark.ondemand.DemandCSPointsTo` (Java,
  available in the `p3ctx` image).
- Before any write-up: search papers citing P3Ctx for prior work on demand-driven L_DCR.

---

## 9. References

- P3Ctx paper: Fig. 1 (kCFA rules), Fig. 2 (L_FC PAG), Fig. 5, Fig. 6 (L_DCR PAG), Eq. 2 (L_C),
  Eq. 8 (L_D), Eq. 15 (bad excursion), Eq. 17 (L_R), Lemma 3/4, Theorem 1, Def. 2.
- Sridharan & Bodík, PLDI 2006, <https://doi.org/10.1145/1133981.1134027>.
- Sridharan, Gopan, Shan, Bodík, OOPSLA 2005, <https://doi.org/10.1145/1094811.1094817>.
- SVF: `svftools/svf` image; CFL grammars in `svf/include/CFL/grammar/`; CMake package in
  `Release-build/lib/cmake/SVF/`.
- Study notes: `../article.md`; talk: `../ldcr-talk/`.
