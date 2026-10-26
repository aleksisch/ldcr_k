cmake_minimum_required(VERSION 3.20)
project(ldc LANGUAGES CXX)
# LAGraph and GraphBLAS are C libraries.
enable_language(C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SVF_DIR / LLVM_DIR / Z3_DIR select native dependencies (see README.md).
find_package(SVF CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
# GraphBLAS_ROOT selects SuiteSparse:GraphBLAS; LAGraph (submodule) brings its CFL solver.
find_package(GraphBLAS CONFIG REQUIRED)
set(BUILD_TESTING OFF CACHE BOOL "LAGraph's own tests" FORCE)
add_subdirectory(third_party/LAGraph EXCLUDE_FROM_ALL)
# LAGraph defaults BUILD_SHARED_LIBS to ON; our libraries stay static, so that SVF (a static
# library) is linked into the program once.
set(BUILD_SHARED_LIBS OFF)

add_library(ldc_frontend
  src/Dispatch.cpp
  src/SimplifiedPAG.cpp
  src/SVFEdges.cpp
  src/SVFFrontend.cpp)
target_compile_features(ldc_frontend PUBLIC cxx_std_20)
target_include_directories(ldc_frontend PUBLIC include)
target_link_libraries(ldc_frontend PUBLIC SVF::SvfLLVM SVF::SvfCore)

# Points-to as CFL reachability: the languages' graphs and grammars, and the solver.
add_library(ldc_cfl src/PagPasses.cpp)
target_link_libraries(ldc_cfl PUBLIC ldc_frontend)
target_sources(ldc_cfl PRIVATE src/Grammar.cpp)
target_sources(ldc_cfl PRIVATE src/Cfl.cpp src/CflSvf.cpp)
target_sources(ldc_cfl PRIVATE src/CflLAGraph.cpp)
target_sources(ldc_cfl PRIVATE src/CflPath.cpp)
target_link_libraries(ldc_cfl PRIVATE LAGraphX LAGraph SuiteSparse::GraphBLAS)

add_executable(ldc src/main.cpp src/CommandLine.cpp)
target_link_libraries(ldc PRIVATE ldc_frontend CLI11::CLI11)
target_sources(ldc PRIVATE src/Facts.cpp)
target_link_libraries(ldc PRIVATE ldc_cfl)
target_sources(ldc PRIVATE src/PagDot.cpp)

include(cmake/LdcIR.cmake)

enable_testing()
add_subdirectory(tests)
