cmake_minimum_required(VERSION 3.20)
project(ldc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SVF_DIR / LLVM_DIR / Z3_DIR come from the environment (see Dockerfile).
find_package(SVF CONFIG REQUIRED)

add_executable(ldc
  src/main.cpp
  src/LDGraph.cpp
  src/Builder.cpp
  src/Names.cpp
  src/Solver.cpp
  src/Check.cpp)
target_include_directories(ldc PRIVATE include)
target_link_libraries(ldc PRIVATE SVF::SvfLLVM SVF::SvfCore)

enable_testing()
add_subdirectory(tests)
