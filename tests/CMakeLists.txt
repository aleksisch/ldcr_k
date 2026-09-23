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
  set(expect "${CMAKE_CURRENT_SOURCE_DIR}/expected/${name}.json")
  # M2: L_FC at k = 0 must equal SVF Andersen on every variable.
  add_test(NAME "${name}.andersen"
           COMMAND ldc -stat=false -ldc-andersen
                   "-ldc-src=${src}" "-ldc-expect=${expect}" "${ll}")
  # M3, M4: the expected queries, per context depth and analysis.
  foreach(k 0 1 2)
    foreach(mode lfc kcfa ldc)
      add_test(NAME "${name}.${mode}.k${k}"
               COMMAND ldc -stat=false -ldc-k=${k} -ldc-mode=${mode}
                       "-ldc-src=${src}" "-ldc-expect=${expect}" "${ll}")
    endforeach()
  endforeach()
endforeach()

add_custom_target(test_ir ALL DEPENDS ${LDC_TEST_IR})
