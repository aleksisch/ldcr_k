# Expected results

One JSON file per `tests/cpp/<name>.cpp`. Checked by CTest from M2 (k = 0) and M3 (k ≥ 1) on.

Naming:
- **objects** — the label in the `// <label>` comment on the allocation line (`oa`, `d1`, `e1`, ...);
- **variables** — `<function>::<name>` with the C++ name (`main::x`, `A::foo::v`);
  `<function>::this` for the receiver.

Each query lists the expected points-to set per mode, where known:
- `kcfa` — the reference kCFA (paper Fig. 1 rules), heap context k−1;
- `lfc`  — baseline L_FC_k (virtual calls wired from Andersen targets as plain edges);
- `ldc`  — L_DC_k (this project, phase 1);
- `ldcr` — L_DCR_k (phase 2); never listed: it must equal `kcfa`.
A mode that is absent means "same as `kcfa`".

`"andersen": false` disables the `<name>.andersen` test (L_FC at k = 0 = SVF Andersen); the
file's `note` says why.
