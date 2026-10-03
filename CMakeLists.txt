cmake_minimum_required(VERSION 3.20)
project(ldc LANGUAGES CXX)

# Optimised by default: the solver is the hot part (without this, CMake builds at -O0).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SVF_DIR / LLVM_DIR / Z3_DIR select native dependencies (see README.md).
find_package(SVF CONFIG REQUIRED)

add_executable(ldc
  src/main.cpp
  src/LDGraph.cpp
  src/Builder.cpp
  src/Solver.cpp
  src/P3Ctx.cpp)
target_include_directories(ldc PRIVATE include)
target_link_libraries(ldc PRIVATE SVF::SvfLLVM SVF::SvfCore)

include(cmake/LdcIR.cmake)

enable_testing()
add_subdirectory(tests)
