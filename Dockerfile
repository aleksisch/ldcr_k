# Development environment: SVF 3.4 + LLVM 21.1.0 (+ clang++ 21), CMake, g++.
# Build and test inside this image:
#   docker build -t ldc-dev .
#   docker run --rm -it -u "$(id -u):$(id -g)" -v "$PWD":/work -w /work ldc-dev
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
FROM svftools/svf:latest

ENV SVF_DIR=/home/SVF-tools/SVF/Release-build/lib/cmake/SVF \
    LLVM_DIR=/home/SVF-tools/SVF/llvm-21.1.0.obj/lib/cmake/llvm \
    Z3_DIR=/home/SVF-tools/SVF/z3.obj

# SVF's exported target SVF::SvfLLVM lists this include directory, but the image does not
# ship it; CMake refuses to generate with a non-existent INTERFACE_INCLUDE_DIRECTORIES entry.
RUN mkdir -p /home/SVF-tools/SVF/Release-build/include/SVF
