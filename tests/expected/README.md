# Expected results

`<name>.k<k>.txt` for `tests/cpp/<name>.cpp` at context depth k: the reference kCFA (paper
Fig. 1 rules, heap context k−1). `<name>.k<k>.<mode>.txt` overrides it for one analysis
(`lfc` — baseline L_FC_k; `ldc` — L_DC_k, phase 1); a mode without a file must match kCFA,
as `ldcr` (L_DCR_k, phase 2) always does.

One line per expected fact, `<variable> -> <object>`, contexts merged; `<variable> ->` means
no objects. Variables not listed are not checked (`tests/expect.sh`).
- **objects** — the label in the `// <label>` comment on the allocation line (`oa`, `d1`, ...);
- **variables** — `<function>::<name>` with the C++ name (`main::x`, `A::foo::v`);
  `<function>::this` for the receiver.

`# andersen: false` in a program's files disables the tests that trust SVF Andersen's call
graph: `<name>.andersen` (L_FC at k = 0 = SVF Andersen) and `<name>.lfc-sound.k<k>`; the
`#` lines above it say why.
