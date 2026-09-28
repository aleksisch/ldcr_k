# Compile every tests/cpp/*.cpp to LLVM IR (clang++ from the SVF image, then mem2reg)
# and register a CTest that runs `ldc` on it.

get_filename_component(_llvm_bin "${LLVM_DIR}/../../../bin" ABSOLUTE)
find_program(LDC_CLANGXX clang++ HINTS "${_llvm_bin}" REQUIRED)
find_program(LDC_OPT opt HINTS "${_llvm_bin}" REQUIRED)

file(GLOB LDC_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cpp/*.cpp")

set(LDC_TEST_IR "")
foreach(src IN LISTS LDC_TEST_SOURCES)
  get_filename_component(name "${src}" NAME_WE)
  set(raw "${CMAKE_CURRENT_BINARY_DIR}/${name}.raw.ll")
  set(ll  "${CMAKE_CURRENT_BINARY_DIR}/${name}.ll")
  add_custom_command(
    OUTPUT "${ll}"
    COMMAND "${LDC_CLANGXX}" -S -g -fno-discard-value-names
            -Xclang -disable-O0-optnone -emit-llvm "${src}" -o "${raw}"
    COMMAND "${LDC_OPT}" -S -p=mem2reg "${raw}" -o "${ll}"
    DEPENDS "${src}"
    COMMENT "IR: ${name}"
    VERBATIM)
  list(APPEND LDC_TEST_IR "${ll}")
  add_test(NAME "${name}" COMMAND ldc "${ll}")
endforeach()

add_custom_target(test_ir ALL DEPENDS ${LDC_TEST_IR})
