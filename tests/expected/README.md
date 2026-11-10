# Expected results

`<name>.txt` for `tests/cpp/<name>.cpp`: the expected points-to facts, one `<variable> -> <object>`
per line; `<variable> ->` means no objects. Variables not listed are not checked
(`tests/expect.sh`).
`<name>.calls.txt`: the same with `--match-calls`.
- **objects** — the label in the `// <label>` comment on the allocation line (`oa`, `d1`, ...);
- **variables** — `<function>::<name>` with the C++ name (`main::x`, `A::foo::v`);
  `<function>::this` for the receiver.
