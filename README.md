# LLVM/SVF infrastructure

A C++17 driver that loads LLVM IR, constructs SVFIR, runs SVF Andersen,
and prints sorted, unique call-graph edges as `call: <caller> -> <callee>`.
Function names use LLVM linkage names. The graph includes direct calls and
indirect/virtual targets resolved by Andersen; its precision is SVF's.

The reusable `ldc_frontend` library also builds an owning `ProgramGraph` with
variable/object nodes, SVF ID lookup, pointer-flow edges, call-site metadata,
and source/debug names. Its headers live in `include/ldc/`.

## Native setup (Ubuntu 24.04, Bash)

### 1. Install build prerequisites

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build git curl wget unzip xz-utils \
  libncurses-dev zlib1g-dev libzstd-dev libffi-dev libxml2-dev
```

CMake 3.23 or newer is needed to build the pinned SVF revision. Keep the same
LLVM installation for building SVF, linking this driver, and producing input IR.

### 2. Build SVF and its dependencies

Use a separate directory for dependencies. These commands pin SVF so that its
C++ API cannot change underneath the project:

```sh
export LDCR_DEPS="$HOME/.local/share/ldcr-deps"
mkdir -p "$LDCR_DEPS"
git clone https://github.com/SVF-tools/SVF.git "$LDCR_DEPS/SVF"
git -C "$LDCR_DEPS/SVF" checkout f78454fb8d71b0d16c80a6f009b8a1d1c20c75e5

(
  cd "$LDCR_DEPS/SVF"
  unset LLVM_DIR Z3_DIR
  SVF_BUILD_JOBS=2 bash ./build.sh
)

# The pinned SVF build exports this include path without creating it.
mkdir -p "$LDCR_DEPS/SVF/Release-build/include/SVF"
```

SVF's [build script](https://github.com/SVF-tools/SVF/blob/f78454fb8d71b0d16c80a6f009b8a1d1c20c75e5/build.sh)
downloads LLVM/Clang 21.1.0 and Z3 4.15.4, then builds SVF locally. It needs
network access and several GB of free disk space. Adjust `SVF_BUILD_JOBS` for
your available memory. Re-running `build.sh` recreates SVF's `Release-build`.

### 3. Configure your shell

Run this in each new shell before configuring or using the driver (or save it
in a shell file and source it):

```sh
export LDCR_DEPS="$HOME/.local/share/ldcr-deps"
export SVF_DIR="$LDCR_DEPS/SVF/Release-build/lib/cmake/SVF"
export LLVM_DIR="$LDCR_DEPS/SVF/llvm-21.1.0.obj/lib/cmake/llvm"
export Z3_DIR="$LDCR_DEPS/SVF/z3.obj"
export PATH="$LDCR_DEPS/SVF/llvm-21.1.0.obj/bin:$PATH"
export LD_LIBRARY_PATH="$LDCR_DEPS/SVF/Release-build/lib:$LDCR_DEPS/SVF/Release-build/svf:$LDCR_DEPS/SVF/Release-build/svf-llvm:$LDCR_DEPS/SVF/llvm-21.1.0.obj/lib:$Z3_DIR/bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

Here `SVF_DIR` and `LLVM_DIR` are directories containing `SVFConfig.cmake` and
`LLVMConfig.cmake`; `Z3_DIR` is the Z3 installation root. SVF's own `setup.sh`
uses different directory conventions, so use the values above for this project.

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
It uses Ubuntu 24.04 and the pinned SVF/LLVM/Z3 bootstrap above, caching the
native toolchain between runs. The first run builds SVF from source.

The tests check direct and resolved function-pointer calls, nested field offsets,
argument/return flow, source/debug information, IR without debug information,
DOT export, and input/output errors. The CMake helper compiles fixtures with
clang++ and runs `mem2reg` with opt from the selected LLVM installation.

## Inspect a call graph

```sh
build-native/ldc -stat=false build-native/tests/call_graph.ll
build-native/ldc -stat=false -dump-callgraph build-native/tests/call_graph.ll
```

SVF's `-dump-callgraph` writes `callgraph_initial.dot` and
`callgraph_final.dot` in the working directory. The latter includes Andersen's
resolved calls. Optionally install Graphviz (`sudo apt install graphviz`) and
render it with `dot -Tsvg callgraph_final.dot -o callgraph.svg`.

To analyze your own C++ source, use the same LLVM tools:

```sh
clang++ -S -emit-llvm -g -fno-discard-value-names \
  -Xclang -disable-O0-optnone example.cpp -o example.raw.ll
opt -S -passes=mem2reg example.raw.ll -o example.ll
build-native/ldc -stat=false -dump-callgraph example.ll
```

Add your program's include paths, language standard, and other compilation flags
as needed. The driver accepts textual IR or bitcode inputs supported by SVF and
releases the analysis, SVFIR, and LLVM module after use.

## Inspect the pointer-flow graph

```sh
build-native/ldc -stat=false -program-dot=program.dot build-native/tests/pointer_flow.ll
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

Link a CMake target against `ldc_frontend`, then use the API while SVF is alive:

```cpp
#include "ldc/SVFFrontend.h"

ldc::frontend::BuildStats stats;
auto graph = ldc::frontend::buildProgramGraph(*pag, *andersen->getCallGraph(), stats);
graph.dumpDot(output);
```

`ProgramGraph` owns its nodes, edges, and metadata, so it remains usable after
SVF/LLVM cleanup. You can construct one directly with `addNode`, `addEdge`, and
`addCallSite`; nodes with an SVF ID are interned, while nodes without one stay
distinct. `findSvf` maps SVF IDs back to graph node IDs. `SVFEdges.h` additionally
exposes the shared statement/call extraction for specialized graph builders;
its results contain non-owning SVF pointers and require SVF to remain alive.

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
  installation. Native validation also passes with the pinned SVF revision and
  LLVM/Clang 22 on Ubuntu 24.04.
- If CMake reports a missing `include/SVF` directory in SVF's build tree, create
  that empty directory as shown in step 2; the actual headers come from SVF's
  source tree and generated include directory.
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
