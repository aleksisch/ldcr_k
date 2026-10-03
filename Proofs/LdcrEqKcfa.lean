/-!
# L_DCR_k = kCFA (PROOF.md §3)

The rule systems 𝓚 (kCFA) and 𝓓 (L_DCR_k) of PROOF.md §2 over an abstract program, and the
two inclusions of the theorem `π(D) = K`:

* `k_sub_piD`  — K ⊆ π(D)  (PROOF.md §3.1),
* `piD_sub_k`  — π(D) ⊆ K  (PROOF.md §3.2).

The shared rules 𝓢 are any monotone rule set over reach / points-to / heap facts. In 𝓓 they
read the points-to facts with tags dropped (`star`), as in the solver. Least models are
intersections of all closed models, so a least model is contained in every model.
-/

namespace Ldcr

/-- The fixed inputs both analyses share (PROOF.md §1). -/
structure Program where
  Var : Type
  Fn : Type
  Site : Type
  Ctx : Type
  Obj : Type
  Ty : Type
  Cell : Type
  /-- `push c C = ⌈c :: C⌉_k`. -/
  push : Site → Ctx → Ctx
  /-- `ν(v, C)`: the context of variable `v` when its function runs in `C`. -/
  nu : Var → Ctx → Ctx
  /-- The function that contains virtual call site `c`. -/
  owner : Site → Fn
  /-- The receiver `r` of `c`. -/
  recv : Site → Var
  /-- The actual `a_i` of `c`. -/
  arg : Site → Nat → Option Var
  /-- The result `x` of `c`. -/
  res : Site → Option Var
  /-- `m' ∈ T(c)` and `m'` is usable. -/
  target : Site → Fn → Prop
  this : Fn → Var
  /-- The formal `p_i` of `m'`. -/
  param : Fn → Nat → Option Var
  ret : Fn → Option Var
  /-- `types(o)`. -/
  types : Obj → Ty → Prop
  /-- `types(c, m')`. -/
  siteTypes : Site → Fn → Ty → Prop

variable (P : Program)

/-- Facts of the shared relations: `Reach`, `Pt`, `Heap`. -/
inductive Base
  | reach (m : P.Fn) (C : P.Ctx)
  | pt (v : P.Var) (C : P.Ctx) (o : P.Obj)
  | heap (κ : P.Cell) (o : P.Obj)

/-- The shared rules 𝓢: `S M f` — some rule concludes `f` from premises in `M`. -/
structure Shared where
  S : (Base P → Prop) → Base P → Prop
  mono : ∀ {M M' : Base P → Prop}, (∀ g, M g → M' g) → ∀ f, S M f → S M' f

variable {P} (Sh : Shared P)

/-! ## kCFA -/

structure KModel where
  base : Base P → Prop
  call : P.Site → P.Ctx → P.Fn → Prop

/-- 𝓚 = 𝓢 ∪ {K-Disp, K-Arg, K-Ret}. -/
structure ClosedK (M : KModel (P := P)) : Prop where
  shared : ∀ f, Sh.S M.base f → M.base f
  disp : ∀ c C m' o t, P.target c m' → M.base (.reach (P.owner c) C) →
    M.base (.pt (P.recv c) (P.nu (P.recv c) C) o) → P.types o t → P.siteTypes c m' t →
    M.base (.reach m' (P.push c C)) ∧ M.base (.pt (P.this m') (P.push c C) o) ∧ M.call c C m'
  arg : ∀ c C m' i a p o', M.call c C m' → P.arg c i = some a → P.param m' i = some p →
    M.base (.pt a (P.nu a C) o') → M.base (.pt p (P.push c C) o')
  ret : ∀ c C m' x r o', M.call c C m' → P.res c = some x → P.ret m' = some r →
    M.base (.pt r (P.push c C) o') → M.base (.pt x (P.nu x C) o')

/-- The least model of 𝓚. -/
def K : KModel (P := P) where
  base f := ∀ M, ClosedK Sh M → M.base f
  call c C m' := ∀ M, ClosedK Sh M → M.call c C m'

theorem K_closed : ClosedK Sh (K Sh) where
  shared f h M hM := hM.shared f (Sh.mono (fun _ hg => hg M hM) f h)
  disp c C m' o t ht hr hp hto hts :=
    ⟨fun M hM => (hM.disp c C m' o t ht (hr M hM) (hp M hM) hto hts).1,
     fun M hM => (hM.disp c C m' o t ht (hr M hM) (hp M hM) hto hts).2.1,
     fun M hM => (hM.disp c C m' o t ht (hr M hM) (hp M hM) hto hts).2.2⟩
  arg c C m' i a p o' hc ha hp hpt M hM := hM.arg c C m' i a p o' (hc M hM) ha hp (hpt M hM)
  ret c C m' x r o' hc hx hr hpt M hM := hM.ret c C m' x r o' (hc M hM) hx hr (hpt M hM)

/-! ## L_DCR_k -/

structure DModel where
  base : Base P → Prop
  call : P.Site → P.Ctx → P.Fn → Prop
  /-- `Pt(r#c, ν(r, C), o)`. -/
  copy : P.Site → P.Ctx → P.Obj → Prop
  /-- `PtT(this^{m'}, push c C, o, (c, C))`. -/
  ptT : P.Fn → P.Obj → P.Site → P.Ctx → Prop
  /-- `IH(i, (c, C), o')`. -/
  ih : Nat → P.Site → P.Ctx → P.Obj → Prop
  /-- `IH(ret, (c, C), o')`. -/
  ihRet : P.Site → P.Ctx → P.Obj → Prop

/-- `Pt*`: points-to facts with tags dropped. -/
def star (M : DModel (P := P)) : Base P → Prop
  | .pt v X o => M.base (.pt v X o) ∨ ∃ m c C, v = P.this m ∧ X = P.push c C ∧ M.ptT m o c C
  | f => M.base f

/-- 𝓓 = 𝓢 ∪ {D-Copy, D-Disp, D-StArg, D-LdArg, D-StRet, D-LdRet}. -/
structure ClosedD (M : DModel (P := P)) : Prop where
  shared : ∀ f, Sh.S (star M) f → M.base f
  copy : ∀ c C o, M.base (.reach (P.owner c) C) → star M (.pt (P.recv c) (P.nu (P.recv c) C) o) →
    M.copy c C o
  disp : ∀ c C m' o t, P.target c m' → M.base (.reach (P.owner c) C) → M.copy c C o →
    P.types o t → P.siteTypes c m' t →
    M.base (.reach m' (P.push c C)) ∧ M.ptT m' o c C ∧ M.call c C m'
  stArg : ∀ c C i a o o', M.base (.reach (P.owner c) C) →
    star M (.pt (P.recv c) (P.nu (P.recv c) C) o) → P.arg c i = some a →
    star M (.pt a (P.nu a C) o') → M.ih i c C o'
  ldArg : ∀ m' c C o i a p o', M.ptT m' o c C → P.arg c i = some a → P.param m' i = some p →
    M.ih i c C o' → M.base (.pt p (P.push c C) o')
  stRet : ∀ m' c C o r o', M.ptT m' o c C → P.ret m' = some r →
    star M (.pt r (P.push c C) o') → M.ihRet c C o'
  ldRet : ∀ c C o x o', M.base (.reach (P.owner c) C) →
    star M (.pt (P.recv c) (P.nu (P.recv c) C) o) → P.res c = some x → M.ihRet c C o' →
    M.base (.pt x (P.nu x C) o')

/-- The least model of 𝓓. -/
def D : DModel (P := P) where
  base f := ∀ M, ClosedD Sh M → M.base f
  call c C m' := ∀ M, ClosedD Sh M → M.call c C m'
  copy c C o := ∀ M, ClosedD Sh M → M.copy c C o
  ptT m' o c C := ∀ M, ClosedD Sh M → M.ptT m' o c C
  ih i c C o' := ∀ M, ClosedD Sh M → M.ih i c C o'
  ihRet c C o' := ∀ M, ClosedD Sh M → M.ihRet c C o'

/-- `star` is monotone in the model. -/
theorem star_mono {M M' : DModel (P := P)} (hb : ∀ f, M.base f → M'.base f)
    (ht : ∀ m o c C, M.ptT m o c C → M'.ptT m o c C) : ∀ f, star M f → star M' f
  | .pt _ _ _, h => h.elim (fun h => .inl (hb _ h))
      fun ⟨m, c, C, hv, hX, hT⟩ => .inr ⟨m, c, C, hv, hX, ht _ _ _ _ hT⟩
  | .reach _ _, h => hb _ h
  | .heap _ _, h => hb _ h

theorem star_D {M} (hM : ClosedD Sh M) : ∀ f, star (D Sh) f → star M f :=
  star_mono (fun _ h => h M hM) (fun _ _ _ _ h => h M hM)

theorem D_closed : ClosedD Sh (D Sh) where
  shared f h _ hM := hM.shared f (Sh.mono (star_D Sh hM) f h)
  copy c C o hr hp M hM := hM.copy c C o (hr M hM) (star_D Sh hM _ hp)
  disp c C m' o t ht hr hc hto hts :=
    ⟨fun M hM => (hM.disp c C m' o t ht (hr M hM) (hc M hM) hto hts).1,
     fun M hM => (hM.disp c C m' o t ht (hr M hM) (hc M hM) hto hts).2.1,
     fun M hM => (hM.disp c C m' o t ht (hr M hM) (hc M hM) hto hts).2.2⟩
  stArg c C i a o o' hr hp ha hpa M hM :=
    hM.stArg c C i a o o' (hr M hM) (star_D Sh hM _ hp) ha (star_D Sh hM _ hpa)
  ldArg m' c C o i a p o' hT ha hp hih M hM := hM.ldArg m' c C o i a p o' (hT M hM) ha hp (hih M hM)
  stRet m' c C o r o' hT hr hp M hM := hM.stRet m' c C o r o' (hT M hM) hr (star_D Sh hM _ hp)
  ldRet c C o x o' hr hp hx hih M hM :=
    hM.ldRet c C o x o' (hr M hM) (star_D Sh hM _ hp) hx (hih M hM)

/-- Inversion: a call edge of D comes from D-Disp, whose receiver copy comes from D-Copy
(the only rules that produce `Call` and `Pt(r#c, …)`). -/
theorem D_call_inv {c C m'} (h : (D Sh).call c C m') :
    ∃ o, (D Sh).base (.reach (P.owner c) C) ∧
      star (D Sh) (.pt (P.recv c) (P.nu (P.recv c) C) o) ∧ (D Sh).ptT m' o c C := by
  -- D restricted to the call edges and receiver copies that have these witnesses is closed.
  let M : DModel (P := P) :=
    { D Sh with
      call := fun c C m' => ∃ o, (D Sh).base (.reach (P.owner c) C) ∧
        star (D Sh) (.pt (P.recv c) (P.nu (P.recv c) C) o) ∧ (D Sh).ptT m' o c C
      copy := fun c C o => (D Sh).base (.reach (P.owner c) C) ∧
        star (D Sh) (.pt (P.recv c) (P.nu (P.recv c) C) o) }
  have hstar : ∀ f, star M f ↔ star (D Sh) f := fun
    | .pt _ _ _ => Iff.rfl
    | .reach _ _ => Iff.rfl
    | .heap _ _ => Iff.rfl
  have hD := D_closed Sh
  have hM : ClosedD Sh M :=
    { shared := fun f h => hD.shared f (Sh.mono (fun g => (hstar g).1) f h)
      copy := fun c C o hr hp => ⟨hr, (hstar _).1 hp⟩
      disp := fun c C m' o t ht hr hc hto hts =>
        have hd := hD.disp c C m' o t ht hr (hD.copy c C o hc.1 hc.2) hto hts
        ⟨hd.1, hd.2.1, o, hc.1, hc.2, hd.2.1⟩
      stArg := fun c C i a o o' hr hp ha hpa =>
        hD.stArg c C i a o o' hr ((hstar _).1 hp) ha ((hstar _).1 hpa)
      ldArg := hD.ldArg
      stRet := fun m' c C o r o' hT hr hp => hD.stRet m' c C o r o' hT hr ((hstar _).1 hp)
      ldRet := fun c C o x o' hr hp hx hih => hD.ldRet c C o x o' hr ((hstar _).1 hp) hx hih }
  exact h M hM

/-! ## The theorem -/

/-- π(D): forget `Pt(r#c, …)` and `IH`, untag. -/
def piD : KModel (P := P) where
  base := star (D Sh)
  call := (D Sh).call

theorem star_of_base {M : DModel (P := P)} : ∀ {f}, M.base f → star M f
  | .pt _ _ _, h => .inl h
  | .reach _ _, h => h
  | .heap _ _, h => h

/-- PROOF.md §3.1: π(D) is a model of 𝓚, hence K ⊆ π(D). -/
theorem piD_closed : ClosedK Sh (piD Sh) where
  shared f h := star_of_base ((D_closed Sh).shared f h)
  disp c C m' o t ht hr hp hto hts :=
    have hd := (D_closed Sh).disp c C m' o t ht hr ((D_closed Sh).copy c C o hr hp) hto hts
    ⟨hd.1, .inr ⟨m', c, C, rfl, rfl, hd.2.1⟩, hd.2.2⟩
  arg c C m' i a p o' hc ha hp hpa :=
    have ⟨o, hr, hrecv, hT⟩ := D_call_inv Sh hc
    star_of_base ((D_closed Sh).ldArg m' c C o i a p o' hT ha hp
      ((D_closed Sh).stArg c C i a o o' hr hrecv ha hpa))
  ret c C m' x r o' hc hx hr hpr :=
    have ⟨o, hreach, hrecv, hT⟩ := D_call_inv Sh hc
    star_of_base ((D_closed Sh).ldRet c C o x o' hreach hrecv hx
      ((D_closed Sh).stRet m' c C o r o' hT hr hpr))

theorem k_sub_piD :
    (∀ f, (K Sh).base f → star (D Sh) f) ∧ (∀ c C m', (K Sh).call c C m' → (D Sh).call c C m') :=
  ⟨fun _ h => h (piD Sh) (piD_closed Sh), fun _ _ _ h => h (piD Sh) (piD_closed Sh)⟩

/-- ε(K): K with the extra relations of 𝓓 defined from it (PROOF.md §3.2). -/
def epsK : DModel (P := P) where
  base := (K Sh).base
  call := (K Sh).call
  copy c C o := (K Sh).base (.reach (P.owner c) C) ∧ (K Sh).base (.pt (P.recv c) (P.nu (P.recv c) C) o)
  ptT m' o c C := (K Sh).base (.reach (P.owner c) C) ∧
    (K Sh).base (.pt (P.recv c) (P.nu (P.recv c) C) o) ∧
    ∃ t, P.target c m' ∧ P.types o t ∧ P.siteTypes c m' t
  ih i c C o' := (K Sh).base (.reach (P.owner c) C) ∧
    (∃ o, (K Sh).base (.pt (P.recv c) (P.nu (P.recv c) C) o)) ∧
    ∃ a, P.arg c i = some a ∧ (K Sh).base (.pt a (P.nu a C) o')
  ihRet c C o' := ∃ m' o r, ((K Sh).base (.reach (P.owner c) C) ∧
    (K Sh).base (.pt (P.recv c) (P.nu (P.recv c) C) o) ∧
    ∃ t, P.target c m' ∧ P.types o t ∧ P.siteTypes c m' t) ∧
    P.ret m' = some r ∧ (K Sh).base (.pt r (P.push c C) o')

/-- Observation: a tagged fact of ε(K) comes from a K-Disp firing in K. -/
theorem epsK_disp {m' o c C} (h : (epsK Sh).ptT m' o c C) :
    (K Sh).base (.pt (P.this m') (P.push c C) o) ∧ (K Sh).call c C m' :=
  have ⟨hr, hp, t, ht, hto, hts⟩ := h
  have hd := (K_closed Sh).disp c C m' o t ht hr hp hto hts
  ⟨hd.2.1, hd.2.2⟩

/-- `Pt*` in ε(K) is `Pt` in K. -/
theorem star_epsK : ∀ f, star (epsK Sh) f → (K Sh).base f
  | .pt _ _ _, .inl h => h
  | .pt _ _ _, .inr ⟨_, _, _, hv, hX, hT⟩ => hv ▸ hX ▸ (epsK_disp Sh hT).1
  | .reach _ _, h => h
  | .heap _ _, h => h

/-- PROOF.md §3.2: ε(K) is a model of 𝓓, hence π(D) ⊆ K. -/
theorem epsK_closed : ClosedD Sh (epsK Sh) where
  shared f h := (K_closed Sh).shared f (Sh.mono (star_epsK Sh) f h)
  copy _ _ _ hr hp := ⟨hr, star_epsK Sh _ hp⟩
  disp c C m' o t ht _ hc hto hts :=
    have hd := (K_closed Sh).disp c C m' o t ht hc.1 hc.2 hto hts
    ⟨hd.1, ⟨hc.1, hc.2, t, ht, hto, hts⟩, hd.2.2⟩
  stArg _ _ _ a o _ hr hp ha hpa := ⟨hr, ⟨o, star_epsK Sh _ hp⟩, a, ha, star_epsK Sh _ hpa⟩
  ldArg m' c C _ i a p o' hT ha hp hih :=
    have ⟨_, _, a', ha', hpa⟩ := hih
    have hEq : a' = a := Option.some.inj (ha'.symm.trans ha)
    (K_closed Sh).arg c C m' i a p o' (epsK_disp Sh hT).2 ha hp (hEq ▸ hpa)
  stRet m' _ _ o r _ hT hr hp := ⟨m', o, r, hT, hr, star_epsK Sh _ hp⟩
  ldRet c C _ x o' _ _ hx hih :=
    have ⟨m', _, r, hT, hr, hpr⟩ := hih
    (K_closed Sh).ret c C m' x r o' (epsK_disp Sh hT).2 hx hr hpr

theorem piD_sub_k :
    (∀ f, star (D Sh) f → (K Sh).base f) ∧ (∀ c C m', (D Sh).call c C m' → (K Sh).call c C m') :=
  ⟨fun f h => star_epsK Sh f (star_D Sh (epsK_closed Sh) f h),
   fun _ _ _ h => h (epsK Sh) (epsK_closed Sh)⟩

/-- **Theorem** (PROOF.md §3): π(D) = K — the same reach, points-to (tags dropped) and heap
facts, and the same virtual call edges. -/
theorem piD_eq_k :
    (∀ f, (K Sh).base f ↔ star (D Sh) f) ∧ (∀ c C m', (K Sh).call c C m' ↔ (D Sh).call c C m') :=
  ⟨fun f => ⟨k_sub_piD Sh |>.1 f, piD_sub_k Sh |>.1 f⟩,
   fun c C m' => ⟨k_sub_piD Sh |>.2 c C m', piD_sub_k Sh |>.2 c C m'⟩⟩

end Ldcr
