# L_DCR_k = kCFA

Claim: on the same LDGraph, the `ldcr` analysis (L_D ∩ C_k ∩ L_R, with L_C and L_R regularised
into one shared context, PLAN.md §7) and the `kcfa` oracle (paper Fig. 1) have **the same
least fixpoint**: the same points-to facts ⟨variable, context⟩ ∋ ⟨object, heap context⟩, the
same heap, the same reached (function, context) pairs and the same virtual call edges.

Three levels, which are not equally strong:

| level | statement | status |
|---|---|---|
| 1. rules | the inference rules of `ldcr` and `kcfa` have the same least model | **proved below** |
| 2. CFL | the rules of `ldcr` compute exactly L_D-reachability on the product graph G × R_k | follows from a standard theorem (§4); checked with an independent matrix solver |
| 3. paper | R_k is the k-limited counterpart of the paper's L_C ∩ L_R (Eq. 2, 17) | by construction, not proved (§5) |

Throughout, "our" kCFA is the oracle in `src/Solver.cpp` (mode `kcfa`); the theorem is
relative to it and to the shared memory model, not to an independent kCFA tool.

## 1. Setting

Fixed inputs, shared by both analyses (built once by `Builder`):

- **Contexts.** `Ctx` = call strings of length ≤ k. `push_c(C) = ⌈c :: C⌉_k`,
  `heap(C) = ⌈C⌉_{k−1}`. Global variables live in the empty context; write `ν(v, C)` for the
  context of variable v when its function runs in C (`[]` for globals, C otherwise).
- **Objects.** `o = ⟨O, h, a⟩`: allocation site O, heap context h, offset a. `types(o)` is
  the set of dynamic types o may dispatch as (`Solver::receiverTypes`: the object's type, a
  member subobject's type, or all member types at offset `*`; empty if unknown).
- **Virtual call sites.** Site c in function m: `x = r.f(a_1, …, a_n)`. Its targets
  `m' ∈ T(c)` (CHA with the declared-type filter), each with a type set `types(c, m')`, formals
  `this^{m'}, p_1^{m'}, …` and return `ret^{m'}`. A target is *usable* if it has formals.
  Write `i ∈ I(c, m')` for the parameter positions `1 ≤ i` with an actual `a_i` and a formal
  `p_i^{m'}`; `R(c, m')` holds if the site has a result x and m' has a `ret`.

Both analyses derive facts of these forms (Horn clauses, i.e. Datalog; least models exist
and are what the worklist solver computes):

- `Reach(m, C)` — function m is analysed in context C;
- `Pt(v, C, o)` — variable v in context C may point to o;
- `Heap(κ, o)` — real heap cell κ (object, field) contains o;
- `Call(c, C, m')` — the virtual call c in context C dispatches to m'.

**Shared rules** 𝓢 (identical code paths in both modes): allocation `[New]`
(`Pt(v, C, ⟨O, heap(ν(v,C)), 0⟩)` when `Reach(m, C)`), assignment, gep, stores and loads of
real fields (with `*` and field limits), direct and indirect non-virtual calls (`Reach` of the
callee in `push_c(C)`, actuals → formals, return → result). Every shared rule that reads a
points-to fact produces **untagged** facts (see §3), and the heap cell of a real-field access
depends only on the object ⟨O, h, a⟩ and the field.

## 2. The two rule systems

### kCFA — 𝓚 = 𝓢 ∪ {K-Disp, K-Arg, K-Ret}

For a virtual site c in m, `X = push_c(C)`, `m'` usable:

```
[K-Disp]  Reach(m,C)   Pt(r, ν(r,C), o)   types(o) ∩ types(c,m') ≠ ∅
          ───────────────────────────────────────────────────────────────
          Reach(m',X)   Pt(this^{m'}, X, o)   Call(c, C, m')

[K-Arg]   Call(c, C, m')   i ∈ I(c,m')   Pt(a_i, ν(a_i,C), o')
          ──────────────────────────────────────────────────
          Pt(p_i^{m'}, X, o')

[K-Ret]   Call(c, C, m')   R(c,m')   Pt(ret^{m'}, X, o')
          ──────────────────────────────────────────────
          Pt(x, ν(x,C), o')
```

(`Solver::virtualCallObject`: the receiver goes to `this` of its own targets; actuals and the
result are wired once per (c, C, target), `wiredTargets_`.)

### L_DCR_k — 𝓓 = 𝓢 ∪ {D-Copy, D-Disp, D-StArg, D-LdArg, D-StRet, D-LdRet}

Extra facts: `Pt(r#c, ν(r,C), o)` for the receiver copy (it lives where r lives); **tagged** facts
`PtT(this^{m'}, X, o, (c, C))`; **instance cells** `IH(i, (c, C), o')` and
`IH(ret, (c, C), o')`. Write `Pt*(v, C, o)` for "`Pt(v, C, o)` or `PtT(v, C, o, τ)` for some τ";
shared rules and the D-rules below read `Pt*` and drop tags.

```
[D-Copy]  Reach(m,C)   Pt*(r, ν(r,C), o)                    ⇒  Pt(r#c, ν(r,C), o)

[D-Disp]  Reach(m,C)   Pt(r#c, ν(r,C), o)   t ∈ types(o) ∩ types(c,m')
          ⇒  Reach(m',X)   PtT(this^{m'}, X, o, (c,C))   Call(c, C, m')

[D-StArg] Reach(m,C)   Pt*(r, ν(r,C), o)   Pt*(a_i, ν(a_i,C), o')      ⇒  IH(i, (c,C), o')
[D-LdArg] PtT(this^{m'}, X, o, (c,C))   i ∈ I(c,m')   IH(i, (c,C), o')  ⇒  Pt(p_i^{m'}, X, o')
[D-StRet] PtT(this^{m'}, X, o, (c,C))   Pt*(ret^{m'}, X, o')           ⇒  IH(ret, (c,C), o')
[D-LdRet] Reach(m,C)   Pt*(r, ν(r,C), o)   R(c,·)   IH(ret, (c,C), o')  ⇒  Pt(x, ν(x,C), o')
```

These are the paper's Fig. 6 edges, `a_i --store[p_i] ⟦ĉ⟧--> r`, `r → r#c`,
`r#c --dispatch[t] ĉ--> this`, `this --load[p_i]--> p_i`, `ret --store[ret]--> this`,
`r --load[ret] ⟦č⟧--> x`, with L_R's DP-C1 / DP-C2 realised as the instance key (c, C): the
store opens instance (c, C) in C, the dispatch in the same C tags the receiver, and loads read
only the instance of the tag. The cell does not name the receiver object; this is the solver's
keying (`cellKey`, `Instance::Edge` / `Instance::Object`). Per-object cells
`IH(o, i, (c,C), o')`, the literal reading of Fig. 6, give the same least model by the same
argument (the construction in §3.2 goes through with `o` added to the key).

## 3. The theorem

Let `K` and `D` be the least models of 𝓚 and 𝓓. Let `π(D)` forget the extra relations
(`Pt(r#c, ·)`, `IH`) and untag: `π(D)` contains `Pt(v, C, o)` iff `Pt*_D(v, C, o)` for a
program variable v, plus `Reach_D`, `Heap_D`, `Call_D`.

> **Theorem.** `π(D) = K`.

Both halves use the same principle: a least model is contained in *every* model of the same
rules. So it suffices to exhibit models.

### 3.1 K ⊆ π(D): π(D) is a model of 𝓚

*Shared rules.* 𝓓 contains 𝓢, and 𝓢 reads `Pt*` and produces untagged facts, so `π(D)` is
closed under 𝓢.

*[K-Disp].* Let `Reach(m,C)`, `Pt*_D(r, ν(r,C), o)`, `t ∈ types(o) ∩ types(c,m')`. [D-Copy]
gives `Pt(r#c, ν(r,C), o)`; [D-Disp] gives `Reach(m',X)`, `PtT(this^{m'}, X, o, (c,C))` — hence
`Pt(this^{m'}, X, o)` in `π(D)` — and `Call(c, C, m')`.

*[K-Arg].* Let `Call_D(c, C, m')`, `i ∈ I(c,m')`, `Pt*_D(a_i, ν(a_i,C), o')`. `Call_D` is only
produced by [D-Disp], so for some o: `Reach(m,C)`, `Pt*(r, ν(r,C), o)` (the only edge into
`r#c` is [D-Copy]) and `PtT(this^{m'}, X, o, (c,C))`. [D-StArg] gives `IH(i, (c,C), o')`;
[D-LdArg] gives `Pt(p_i^{m'}, X, o')`.

*[K-Ret].* Let `Call_D(c, C, m')`, `R(c,m')`, `Pt*_D(ret^{m'}, X, o')`. With the same o,
[D-StRet] gives `IH(ret, (c,C), o')` and [D-LdRet] gives `Pt(x, ν(x,C), o')`.

So `π(D)` satisfies 𝓚, and `K ⊆ π(D)`. ∎

### 3.2 π(D) ⊆ K: an extension of K is a model of 𝓓

Extend `K` to `ε(K)` by *defining* the extra relations from K (`X = push_c(C)`):

- `Pt(r#c, ν(r,C), o)` iff `Reach_K(m,C)` and `Pt_K(r, ν(r,C), o)`;
- `PtT(this^{m'}, X, o, (c,C))` iff `Reach_K(m,C)`, `Pt_K(r, ν(r,C), o)`,
  `types(o) ∩ types(c,m') ≠ ∅`, m' usable;
- `IH(i, (c,C), o')` iff `Reach_K(m,C)`, `∃o. Pt_K(r, ν(r,C), o)`, `Pt_K(a_i, ν(a_i,C), o')`;
- `IH(ret, (c,C), o')` iff `∃m', o. PtT(this^{m'}, X, o, (c,C))` and `Pt_K(ret^{m'}, X, o')`.

**Observation.** If `PtT(this^{m'}, X, o, (c,C))` holds in `ε(K)` then [K-Disp] fired in K:
`Pt_K(this^{m'}, X, o)`, `Reach_K(m',X)`, `Call_K(c, C, m')`. Hence every tagged fact has an
untagged twin in K, and `Pt*` in `ε(K)` coincides with `Pt` in K on program variables.

Now check every rule of 𝓓 on `ε(K)`:

- *Shared rules.* Their premises read `Pt*`, which by the observation equals K's `Pt`; K is
  closed under 𝓢.
- *[D-Copy].* Holds by the definition of `Pt(r#c, …)`.
- *[D-Disp].* Premises give `Reach_K(m,C)`, `Pt_K(r, ν(r,C), o)` and a common type, so [K-Disp]
  gives `Reach_K(m',X)` and `Call_K(c, C, m')`; `PtT(…, (c,C))` holds by definition.
- *[D-StArg], [D-StRet].* Hold by the definitions of `IH`.
- *[D-LdArg].* From `PtT(this^{m'}, X, o, (c,C))`: `Call_K(c, C, m')` (observation). From
  `IH(i, (c,C), o')`: `Pt_K(a_i, ν(a_i,C), o')`. [K-Arg] gives `Pt_K(p_i^{m'}, X, o')`.
- *[D-LdRet].* From `IH(ret, (c,C), o')`: some m', o with `PtT(this^{m'}, X, o, (c,C))` —
  so `Call_K(c, C, m')` — and `Pt_K(ret^{m'}, X, o')`. [K-Ret] gives `Pt_K(x, ν(x,C), o')`.

So `ε(K)` is a model of 𝓓, hence `D ⊆ ε(K)`, and `π(D) ⊆ π(ε(K)) = K` (the added relations
are forgotten by π; tagged facts map to untagged facts already in K). ∎

Both inclusions are checked in Lean 4 (`Proofs/LdcrEqKcfa.lean`, `cd Proofs && lake build`):
`k_sub_piD`, `piD_sub_k` and `piD_eq_k`, over an abstract program in which the shared rules 𝓢
are any monotone rule set that reads `Pt*`. The only axiom used is `propext`.

With 3.1: **`π(D) = K`** — equal points-to facts for every program variable and context, equal
`Heap` (it is derived by the shared rules from equal points-to facts), equal `Reach`, equal
virtual call edges `Call`.

### What the proof uses (and where it would break)

1. **Same targets and types.** Both analyses take T(c), `types(c, m')` and `types(o)` from the
   same graph; [K-Disp] and [D-Disp] then fire on the same receivers.
2. **The only edge into `r#c` is [D-Copy]**, so a dispatch at (c, C) implies a receiver in
   `pts(r)` in C — this is what makes DP-C2 hold.
3. **The instance key contains the caller context C, and `X = push_c(C)` is a function of it.**
   Dropping C from the key (DP-C1 only) breaks 3.2 at [D-LdArg]: `IH(i, c, o')` would contain
   actuals from every caller context of c, while [K-Arg] passes only those of C. That is the
   paper's Eq. 15 loss (`eq15.cpp`). Dropping the key altogether gives L_DC (Eq. 13, 15).
4. **Tags are dropped by every shared rule**, so a tag only selects the instance at
   `this --load[p_i]-->` and `ret --store[ret]--> this`.
5. **Truncation is harmless.** Two caller contexts C₁ ≠ C₂ with `push_c(C₁) = push_c(C₂) = X`
   give two instances; [D-LdArg] merges both into `p_i^{m'}` at X, exactly as [K-Arg] does.

## 4. Level 2: L_DCR_k as CFL-reachability

𝓓 is a *chain* Datalog program over the product graph G × R_k whose nodes are ⟨v, C⟩, the
tag-split ⟨this, X, (c,C)⟩ and objects: every rule joins facts along a path of edges, and the
instance key only renames the synthetic field labels (`store[p_i]` ↦ `store[p_i@(c,C)]`, a
finite alphabet). By the correspondence between chain Datalog and CFL-reachability
(Reps, *Program analysis via graph reachability*, 1998; for field-sensitive points-to as
L_F-reachability, Sridharan & Bodík PLDI '06), `D` is exactly the set of L_D-paths in
G × R_k, with the dispatch type check as finitely many copies of the flowsto nonterminal.

Independent check (earlier commit `1340b62`, `-ldc-export-cfl`): the product graph and an
L_D grammar were exported and solved by the matrix-based all-pairs solver of
Muravev & Grigorev (SOAP '25). The answers were identical to `ldc`'s pair by pair on all unit
tests and small programs (ldcr, ldc, k = 0–2) and on tinyxml2 at k = 0 (2 677 849 pairs).

## 5. Level 3: what is not proved

- **The paper's L_DCR is unbounded** (no k). The statement "L_DCR restricted to k-limited
  contexts = kCFA" needs a path-level definition of that restriction. R_k — states
  ⟨context, instance tag⟩ with kCFA's truncation — is *our* choice of it; that it is the
  natural k-limit of the paper's grammar (Eq. 17) is by construction, not by proof. The
  paper's own equivalence result should be compared with this.
- **Independent limits** (L_C and L_R truncated separately) are not covered; §3 point 3
  predicts they differ from kCFA.
- **Soundness w.r.t. C++ execution** is a different question: both analyses share the
  memory model, the type recovery and the call targets built by `Builder`.

## 6. Empirical agreement

The `-ldc-facts` files of `ldcr` and `kcfa` are equal, fact by fact, and CTest gates on it
(`<program>.ldcr-same.k<k>`): all unit tests, the three small programs at k = 0–4, and
tinyxml2 at k = 0 and 1 (RESULTS.md). A negative control (L_DC in place of L_DCR, k = 2)
reports differences (2 / 10 / 20 619 facts on `fig8` / `eq15` / `expr`), as §3 point 3
predicts.
