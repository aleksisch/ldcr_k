# CFL reachability for C++

A C++20 project for field- and call-site-sensitive pointer analysis through
context-free-language (CFL) reachability. Currently, `ldc` loads LLVM IR through
SVF, runs Andersen to resolve call targets, and exports a simplified pointer
assignment graph. The CFL analysis is planned as a follow-up.

## Setup

On Ubuntu 24.04 x86-64, install prerequisites and download the pinned binaries:

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build curl unzip xz-utils \
  libncurses-dev zlib1g-dev libzstd-dev libffi-dev libxml2-dev libcli11-dev
bash scripts/setup-deps.sh
```

The helper verifies and installs SVF, LLVM 21.1.0, and Z3 4.15.4 locally.
Source its environment in each new Bash shell:

```sh
source "$HOME/.local/share/ldcr-deps/env.sh"
```

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSVF_DIR="$SVF_DIR" -DLLVM_DIR="$LLVM_DIR" -DZ3_DIR="$Z3_DIR"
cmake --build build --parallel 2
CTEST_OUTPUT_ON_FAILURE=1 cmake --build build --target test
```

For existing installations, adjust the three package paths. Use the LLVM version
SVF was built against, including its `clang++` and `opt`. Build before running
`test`; the test target does not compile binaries or IR fixtures.
[CI](.github/workflows/ci.yml) runs the same build and tests.

## Usage

Compile a source file to LLVM IR, then analyze it:

```sh
clang++ -S -emit-llvm -g -fno-discard-value-names \
  -Xclang -disable-O0-optnone example.cpp -o example.raw.ll
opt -S -passes=mem2reg example.raw.ll -o example.ll
build/ldc example.ll
build/ldc --program-dot program.dot example.ll
build/ldc --help
```

Add your program's include paths and compilation flags as needed. To try the
built-in example, use `build/tests/pointer_flow.ll`. To render DOT, install
Graphviz and run `dot -Tsvg program.dot -o program.svg`.

The tool prints sorted `call: <caller> -> <callee>` edges using LLVM linkage
names. DOT output includes pointer-flow edges, call sites, and source metadata.
GEP chains are folded into load/store field offsets; escaping field addresses
retain explicit GEP edges.

## Library

Link against `ldc_frontend` and include [ldc/SVFFrontend.h](include/ldc/SVFFrontend.h).
`ldc::frontend::analyzeModules({"input.ll"})` returns an owned graph, call edges,
and statistics after releasing LLVM/SVF. Analysis uses process-global SVF state,
so sessions must not overlap.

[SimplifiedPAG](include/ldc/SimplifiedPAG.h) exposes `nodes()`, `edges()`,
`callSites()`, and `dumpDot()`. For an existing SVF session, use
`buildSimplifiedPAG()` directly.
