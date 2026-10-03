# LLVM/SVF infrastructure

A C++17 driver that loads LLVM IR, constructs SVFIR, runs SVF Andersen,
and prints sorted, unique call-graph edges as `call: <caller> -> <callee>`.
Function names use LLVM linkage names. The graph includes direct calls and
indirect/virtual targets resolved by Andersen; its precision is SVF's.

## Build and test

The Dockerfile provides the SVF/LLVM toolchain and CMake package paths:

```sh
docker build -t ldc-dev .
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/work -w /work ldc-dev \
  sh -c 'cmake -S . -B build && cmake --build build -j2 && ctest --test-dir build --output-on-failure'
```

`cmake/LdcIR.cmake` compiles C++ to LLVM IR with debug information and runs
`mem2reg` using the image's clang++ and opt. Tests check direct and resolved
function-pointer edges, DOT exports, and missing-input failure.

## Inspect a call graph

Inside the development image:

```sh
build/ldc -stat=false build/tests/call_graph.ll
build/ldc -stat=false -dump-callgraph build/tests/call_graph.ll
```

SVF's `-dump-callgraph` writes `callgraph_initial.dot` and
`callgraph_final.dot` in the working directory. The latter includes Andersen's
resolved calls. The driver accepts LLVM textual IR or bitcode inputs supported
by SVF. It releases the analysis, SVFIR, and LLVM module after use.

The repository's existing PLAN.md describes the larger research project.
LDCR graph construction, context-sensitive solvers, proofs, and evaluation are
reserved for a follow-up change.
