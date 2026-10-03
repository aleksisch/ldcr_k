cmake_minimum_required(VERSION 3.20)
project(ldc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SVF_DIR / LLVM_DIR / Z3_DIR select native dependencies (see README.md).
find_package(SVF CONFIG REQUIRED)

add_library(ldc_frontend
  src/ProgramGraph.cpp
  src/SVFEdges.cpp
  src/SVFFrontend.cpp)
target_compile_features(ldc_frontend PUBLIC cxx_std_17)
target_include_directories(ldc_frontend PUBLIC include)
target_link_libraries(ldc_frontend PUBLIC SVF::SvfLLVM SVF::SvfCore)

add_executable(ldc src/main.cpp)
target_link_libraries(ldc PRIVATE ldc_frontend)

include(cmake/LdcIR.cmake)

enable_testing()
add_subdirectory(tests)
