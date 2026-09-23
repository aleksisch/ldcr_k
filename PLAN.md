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

Phase 2: add L_R → **L_DCR_k** (§7; implemented, equals kCFA on all programs).

---

## 1. Why L_DC first

| | What it fixes | What it still gets wrong |
|---|---|---|
| L_FC (baseline, = SVF `CFLAlias`, Soot `DemandCSPointsTo`; SVF's Andersen behaves the same, see §6) | fields, contexts | virtual calls: receiver objects cross to the wrong target (paper Fig. 5) |
| **L_DC_k** (phase 1) | + receiver → only its own target (Lemma 3), sound parameter passing (Lemma 4) | dispatch excursion may return under the wrong context (paper Eq. 15) |
| L_DCR_k (phase 2) | + excursion returns the same way (DP-C1, DP-C2) | — (measured: equal to kCFA, §7) |

L_DC is sound but not precise (paper Def. 2, Lemma 4). That is fine for phase 1: it already
beats L_FC on the Fig. 5 case, and the Eq. 15 case becomes the motivating test for phase 2.
L_DC and L_FC are **incomparable**: on Fig. 8 (DP-C1) L_FC with the Andersen call graph is
exact and L_DC is not (see §6, M4 findings).

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
| Test front end | `clang++` 21 from the image; `-O0 -Xclang -disable-O0-optnone -fno-discard-value-names -g -flto -fwhole-program-vtables`, then `opt -p=mem2reg` (`cmake/LdcIR.cmake`) |
| Dispatch candidates | L_D and kCFA: CHA from the vtable slots (as the paper). L_FC: SVF Andersen call graph |
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
`ldc [-ldc-dot=<out>] [-ldc-k=<n>] [-ldc-mode={lfc,kcfa,ldc}] [-ldc-src=<cpp>]
[-ldc-expect=<json>] [-ldc-andersen] [-ldc-eval] <file.ll>`;
to come: `-ldc-query=<var>`, `-ldc-json=<out>`.

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

**Done when** (revised after implementing; status: **done**):
- `fields.cpp`, `static_calls.cpp` (k = 0, 1, 2), `fig3.cpp` (k = 1), `eq15.cpp` (k = 1, 2):
  L_FC_k = kCFA (validates C_k and heap context k−1);
- `fig5.cpp` (k = 0): L_FC_k ⊋ kCFA — the Fig. 5 loss is reproduced;
- `fig3.cpp` (k = 2): L_FC_k ⊋ kCFA — a second loss, from the context-insensitive call graph
  (see §6, M3 findings).

CTest runs every test as `<name>.andersen` (M2) and `<name>.<lfc|kcfa>.k<0|1|2>`.

### M4 — Dispatch: L_D ∩ C_k  (= L_DC_k)  — status: **done**
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
- added `fig8.cpp` (paper Fig. 8, DP-C1): L_DC_k ⊋ kCFA at every k.

CTest adds `<name>.ldc.k<0|1|2>`.

### M5 — Evaluation (phase 1)  — status: **done**, see `RESULTS.md`
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
| Object type recovery | the class of the constructor called directly on the allocated pointer (see §6, M3 findings) |
| Declared type of a virtual call | clang `-fwhole-program-vtables` type tests (see §6, M5 findings) |
| Stack / global objects with virtual methods | allocation site = `alloca` / global, type from ctor |
| Arrays, unions, casts | SVF's field model: array elements collapse onto the array's field; pointer arithmetic over struct fields = field `*`; casts = `assign` |
| Interior pointers (`&o->member`) | field objects ⟨O, off⟩ via `gep[f]` edges (like SVF `GepObjVar`) |
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
- **Escaping geps** (a gep result used as a value — in C++ mostly the vptr `&vtable[2]` stored by
  constructors) are modelled as `base --assign--> gep`, dropping the offset. With that, an object's
  field 0 holds its class's vtable.
- **Noise** kept in LDGraph for now: vtable / typeinfo globals and function objects.

Findings from M2:
- On all five tests our k = 0 solver on LDGraph gives **exactly** SVF Andersen's PTS for every
  variable (compared on base objects).
- **SVF's Andersen has the paper's Fig. 5 defect.** On `fig5.cpp` it gives `this` of `E::foo` =
  `{e1, f1}`, the same as our L_FC wiring: it passes the receiver to `this` of every target like an
  ordinary argument. So SVF Andersen is itself an L_FC-style baseline for M5, not only a sanity check.
- ~~Type recovery for M4 can read the vtable from field 0.~~ Wrong, see M3 findings.

Findings from M3:
- **SVF's CHG gives no virtual targets** in this build (`getVFnsFromVtbls` is empty). `Builder`
  reads the vtables itself: every LLVM global `_ZTV*` with an initializer; the slot for a call
  site is initializer element `2 + getFunIdxInVtable()` (Itanium address point 2). The SVF object
  of a vtable is named after the class ("A"), not `_ZTV1A`, so it is found through
  `LLVMModuleSet::getObjectNode(global)`, not by name.
- **Field 0 does not give the dynamic type.** Flow-insensitively, field 0 of an `F` object holds
  both `vtable for F` and `vtable for E`: `F::F` calls `E::E`, which stores its own vptr first.
  So `DynTypeOf(O)` is set at allocation (as paper [C-New]): the class of the constructor whose
  `this` receives the allocated pointer directly (base constructors get `this`, not the
  allocation). `Builder::assignTypes` stores it on the object node and on its `new` edge.
- **k matters for the examples.** `eq15.cpp` needs k = 2 even for kCFA: at k = 1 both calls of
  `id` run in `[c8]`. `fig3.cpp` needs k = 2 (heap context of `d1`).
- **L_FC has a second loss besides Fig. 5**, on `fig3.cpp` at k = 2: the call graph edge
  `c3 → A::foo` comes from context-insensitive Andersen, so L_FC also enters `A::foo` under
  `[c3, c2]`, where only `b1` is the receiver; `o2` then reaches `A::foo::v`. kCFA does not.
  L_D fixes both (the edge is taken only under `new[A]`).

Findings from M4:
- **Edges** (`Builder::addDispatch`, encoding `D`): `a_i --store[p_i] ⟦ĉ⟧--> r`,
  `r --assign--> r#c`, `r#c --dispatch[t] ĉ--> this^m'`, `this^m' --load[p_i]--> p_i`,
  `ret^m' --store[ret]--> this^m'`, `r --load[ret] ⟦č⟧--> x`. The Andersen-wired edges of the
  same call are encoding `Fc`; each mode uses only its own (`Solver::uses`). The paper's second,
  boxed `r --assign ⟦č⟧--> r#c` is the same fact for L_D ∩ C_k; it is left for phase 2.
- **Synthetic fields** `p_i` / `ret` are separate field ids (the paper's offsets `i` / `0` would
  clash with real fields: field 0 is the vptr). `load[*]` does not read them.
- **In inclusion form, L_DC_k is simple**: `dispatch[t]` = [I-VCall] for the receiver; arguments
  and the result meet through the field `p_i` / `ret` of the heap object ⟨O, h⟩. Its losses vs
  kCFA are exactly the paper's two: the field is shared by **all call sites** where O is the
  receiver (Eq. 13, DP-C1: `fig8`) and by **all caller contexts** that map to the same heap
  context h (Eq. 15, DP-C2: `eq15`, J1 has h = [] while the calls run in [c6] and [c7]).
- **CHA targets** from vtable slots (no declared-type filter): `fig3` has 3 (`C::foo` included);
  L_D never reaches `C::foo`, since no object of type C flows to `x`.
- L_DC_k = kCFA on `static_calls`, `fields`, `fig3`, `fig5` for k = 0, 1, 2.

Findings from M5 (numbers in `RESULTS.md`):
- **DeclTypeOf(r) is required.** Slot-only CHA sends a call to methods of unrelated
  hierarchies that share the slot index (`root->handle(e)` → `Copy::run`). SVF's
  `getFunNameOfVirtualCall()` is empty: it reads "VCallFunName" metadata that only SVF's
  preprocessing adds — also the reason SVF's CHG found no targets (M3). Instead, IR is compiled
  with `-fwhole-program-vtables`, and `Builder::declaredClass` reads the class from
  `llvm.public.type.test(%vtable, !"_ZTS<class>")`; the hierarchy comes from debug info
  (`DW_TAG_inheritance`).
- **Memory model fixes** (all analyses): array index → array's field (`getConstantStructFldIdx`),
  only variant-field geps → `*`; escaping geps → `gep[f]` edges and objects ⟨O, h, off⟩ in the
  solver. Dropping the offset merged `bus.history.items` with `bus.root`.
- **L_FC k = 0 = Andersen holds on the unit tests only**; on the evaluation programs 10–61
  variables differ (memory-model details). kCFA ⊆ Andersen on all three programs.
- **Main result:** L_DC ⊇ kCFA always; L_DC removes 23–100 % of L_FC's extra objects; the
  remaining loss is dominated by DP-C1 in the visitor pattern (`expr`).

---

## 7. Phase 2 — L_R → L_DCR_k  — status: **implemented** (`-ldc-mode=ldcr`)

Same LDGraph as L_DC (encoding `D`); only the solver differs. With the **single shared
context** of C_k, L_R's conditions become checks on solver state:

| paper (Eq. 16, 17) | solver (`Solver::storeInstance` / `loadInstance`, dispatch in `applyCall`) |
|---|---|
| `a_i --store[p_i] ⟦ĉ_c⟧--> r` opens a dispatch path in context C | the argument goes to O.p_i **of instance (c, C)**, for each O ∈ pts(r, C) |
| `r --assign ⟦č_c⟧--> r#c --dispatch[t] ĉ_c--> this` closes it | the receiver fact entering `this` in ⌈c :: C⌉_k is tagged (c, C) |
| DP-C1: closes at the same site c | `this --load[p_i]-->` reads only the instance of the tag: same c … |
| DP-C2: O pointed to by r under the same context | … and same C |
| returns (mirror) | `ret --store[ret]--> this` writes the tag's instance; `r --load[ret] ⟦č_c⟧--> x` in C reads (c, C) |

Tags live only in `this` of the dispatched method; every other edge drops them.

**Result:** L_DCR_k = kCFA **fact by fact** — the same (object, heap context) set for every
(variable, context) — on all unit tests and all evaluation programs for k = 0…4
(`ldc -ldc-eval` checks it; CTest `<program>.eval.k<k>` fails otherwise). A negative control
(comparing L_DC instead) reports 2 / 10 / 2613 differing pairs on `fig8` / `eq15` / `expr`.

**Proof sketch** (inclusion form, induction on derivations; all other rules are shared):
- kCFA ⊆ L_DCR: an [I-VCall] firing for (c, C, O ∈ pts(r, C), m′ = dispatch(c, type O)) adds
  O to this^m′, pts(a_i, C) to p_i^m′ and ret^m′ to x in C, with callee context ⌈c :: C⌉_k.
  L_DCR derives the same: dispatch adds O^(c,C) to this^m′; storeInstance puts pts(a_i, C)
  into O.p_i@(c, C); loadInstance moves it to p_i^m′; the return goes through O.ret@(c, C).
- L_DCR ⊆ kCFA: O.p_i@(c, C) is written only from pts(a_i, C) with O ∈ pts(r, C), and read only
  in m′ through the tag (c, C), which exists only if dispatch fired for O at (c, C) — exactly
  the premise of [I-VCall]. Returns are symmetric. Receivers of unknown type are dropped by both.

So in inclusion form the equality is close to *by construction*. The open part — the
research question — is the CFL side: that this solver computes exactly the paths accepted by
L_D ∩ C_k ∩ L_R_k with the shared-context regularisation (path ↔ derivation), and a
demand-driven (single-query) solver for it, which is where CFL pays off over kCFA (§8).

Not done: the planned experiment with **independent** k-limiting of L_C and L_R (expected to
differ from kCFA).

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
