# LLVM/SVF infrastructure

A C++17 driver that loads LLVM IR, constructs SVFIR, runs SVF Andersen,
and prints sorted, unique call-graph edges as `call: <caller> -> <callee>`.
Function names use LLVM linkage names. The graph includes direct calls and
indirect/virtual targets resolved by Andersen; its precision is SVF's.

The reusable `ldc_frontend` library also builds an owning simplified pointer
assignment graph (`SimplifiedPAG`) with variable/object nodes, SVF ID lookup,
pointer-flow edges, call-site metadata,
and source/debug names. Its headers live in `include/ldc/`.

## Native setup (Ubuntu 24.04 x86-64, Bash)

### 1. Install build prerequisites

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build git curl wget unzip xz-utils \
  libncurses-dev zlib1g-dev libzstd-dev libffi-dev libxml2-dev
```

Keep the same LLVM installation for linking this driver and producing input IR.
The packages below include SVF's headers, libraries, CMake files, and LLVM tools.

### 2. Install prebuilt dependencies

From this repository checkout:

```sh
export LDCR_DEPS="$HOME/.local/share/ldcr-deps"
bash scripts/setup-deps.sh "$LDCR_DEPS"
```

The script downloads and verifies pinned SHA-256 checksums for the official
[SVF Ubuntu package and LLVM 21.1.0](https://github.com/SVF-tools/SVF/releases/tag/SVF-3.3)
and [Z3 4.15.4](https://github.com/Z3Prover/z3/releases/tag/z3-4.15.4).
It unpacks them locally; no dependency compilation or Docker is needed. Allow
several GB of free disk space. The packages target Ubuntu 24.04 x86-64 (Z3 needs
glibc 2.39 or newer). The SVF release is tagged `SVF-3.3`, while its packaged
CMake metadata reports version 3.4; use the exact matching archives in the script.

### 3. Configure your shell

Run this in each new Bash shell before configuring or using the driver:

```sh
export LDCR_DEPS="$HOME/.local/share/ldcr-deps"
source "$LDCR_DEPS/env.sh"
```

The generated environment sets `SVF_DIR` and `LLVM_DIR` to their CMake package
directories, `Z3_DIR` to the Z3 installation root, and the compiler/runtime
library paths. Use the same dependency directory you passed to the installer.

### 4. Build and test this repository

From your repository checkout:

```sh
cmake -S . -B build-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSVF_DIR="$SVF_DIR" -DLLVM_DIR="$LLVM_DIR" -DZ3_DIR="$Z3_DIR"
cmake --build build-native --parallel 2
CTEST_OUTPUT_ON_FAILURE=1 cmake --build build-native --target test
```

`test` is CMake's standard test target. Build the default `all` target first as
shown above: `test` runs CTest but does not build test binaries or IR fixtures.
`CTEST_OUTPUT_ON_FAILURE=1` displays diagnostic output for failing tests.
You can also run CTest directly with `ctest --test-dir build-native --output-on-failure`.

[GitHub Actions](.github/workflows/ci.yml) runs the same native build and `test`
target on pull requests and pushes to `master`, and supports manual runs.
It uses Ubuntu 24.04 and the pinned prebuilt SVF/LLVM/Z3 packages above, caching
the native toolchain between runs. Even the first run uses binary dependencies.

The tests check direct and resolved function-pointer calls, nested field offsets,
argument/return flow, source/debug information, IR without debug information,
DOT export, and input/output errors. The CMake helper compiles fixtures with
clang++ and runs `mem2reg` with opt from the selected LLVM installation.

## Inspect a call graph

```sh
build-native/ldc build-native/tests/call_graph.ll
build-native/ldc --help
```

The tool prints sorted caller/callee pairs, including resolved indirect calls.
`--help` lists only the options implemented by `ldc`.

To analyze your own C++ source, use the same LLVM tools:

```sh
clang++ -S -emit-llvm -g -fno-discard-value-names \
  -Xclang -disable-O0-optnone example.cpp -o example.raw.ll
opt -S -passes=mem2reg example.raw.ll -o example.ll
build-native/ldc example.ll
```

Add your program's include paths, language standard, and other compilation flags
as needed. The driver accepts textual IR or bitcode inputs supported by SVF and
releases the analysis, SVFIR, and LLVM module after use.

## Inspect the pointer-flow graph

```sh
build-native/ldc --program-dot program.dot build-native/tests/pointer_flow.ll
```

`program.dot` contains variable/object nodes, source names and locations, and
`new`, `assign`, `store[field]`, `load[field]`, and `gep[field]` edges. Call
argument and return edges carry `enter@site` / `exit@site` labels. Separate
call-site notes list resolved targets, including calls without pointer flow.

The frontend folds loads/stores through GEPs onto their base pointer and flattened
struct offset. Escaping field addresses retain explicit `gep` edges. Phi/select
inputs become assignment edges. Indirect and virtual call connections come from
Andersen's resolved call graph; unresolved call sites remain present with no
targets. This does not perform a new points-to analysis or infer missing targets.

### Use the library

Link a CMake target against `ldc_frontend`. For a complete run from LLVM IR,
`analyzeModules({"input.ll"})` loads the modules, runs Andersen, and returns an
owned `FrontendResult` containing the simplified PAG, call edges, and statistics.
SVF/LLVM are released before it returns. It uses process-global state and cannot
overlap another SVF session.

If you already manage SVF yourself, build the simplified PAG while SVF is alive:

```cpp
#include "ldc/SVFFrontend.h"

ldc::frontend::BuildStats stats;
auto graph = ldc::frontend::buildSimplifiedPAG(*pag, *andersen->getCallGraph(), stats);
graph.dumpDot(output);
```

`SimplifiedPAG` owns its nodes, edges, and metadata, so it remains usable after
SVF/LLVM cleanup. You can construct one directly with `addNode`, `addEdge`, and
`addCallSite`; nodes with an SVF ID are interned, while nodes without one stay
distinct. `findSvf` maps SVF IDs back to graph node IDs. Read the graph through
`nodes()`, `edges()`, and `callSites()`; export it with `dumpDot()` or inspect
counts with `printSummary()`.

The public headers are `SimplifiedPAG.h` (owned graph records and operations) and
`SVFFrontend.h` (module analysis, graph extraction, and result statistics). Statement/call extraction
and debug-name helpers are private implementation details in `src/`.

Node, SVF, and call-site IDs are distinct structs with a `value` member;
integer conversion and vector indexing are explicit. `FieldOffset` is a separate
signed offset type. Call-site properties are packed bit-fields accessed through
`site.flags.isIndirect` and `site.flags.isVirtual`, both initially false.

Missing SVF IDs, edge fields, and call-site IDs use `std::optional`, rather than
reserved integer values. The one field constant, `kAnyField`, represents an
unknown offset; it differs from an absent field and from offset zero.

Each call site records its caller, source location, direct/indirect/virtual flags,
actual arguments, return value, and resolved targets with their formals/return.
Missing argument nodes use empty optionals to preserve parameter positions.
Function names in this API are demangled; the driver's `call:` lines retain LLVM
linkage names. Source locations retain SVF's location text plus the line number.
Debug records supply a source variable name when possible; aliases of one SSA
value share the first recovered name. Without debug information the graph uses
IR/generated names and line 0.

The extraction retains the original frontend's abstraction: constants, dummy
nodes, analysis-created field objects, and intrinsic-local values are excluded;
array indices follow SVF's abstraction and unknown field offsets use `*`.
Skipped edges and variable-offset GEPs are reported. Class hierarchy/vtable
modeling and LDCR dispatch labels are left to the follow-up analysis.

## Existing installations and troubleshooting

- You can reuse an existing SVF build: point `SVF_DIR` at its CMake package,
  `LLVM_DIR` at the LLVM package used to build it, and `Z3_DIR` at its Z3
  installation. The frontend also supports the previously tested SVF source
  revision `f78454fb8d71b0d16c80a6f009b8a1d1c20c75e5` with LLVM/Clang 22.
- If CMake reports a missing `include/SVF` directory in SVF's build tree, create
  that empty directory; the actual headers come from SVF's source tree and
  generated include directory. The prebuilt setup does not need this workaround.
- If clang++ or opt is missing, install both for the selected LLVM version.
  For a custom layout, pass `-DLDC_CLANGXX=/absolute/path/to/clang++` and
  `-DLDC_OPT=/absolute/path/to/opt` when configuring. Do not mix LLVM versions.
- If shared libraries cannot be loaded, reapply the shell environment in step 3;
  for a custom installation, use its SVF, LLVM, and Z3 library directories.
- After changing dependency paths or compiler versions, use a fresh build
  directory so that CMake does not reuse stale cached paths.

The repository's existing PLAN.md describes the larger research project.
LDCR graph construction, context-sensitive solvers, proofs, and evaluation are
reserved for a follow-up change.
