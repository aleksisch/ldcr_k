# Compile every tests/cpp/*.cpp to LLVM IR and register CTests that run `ldc` on it.

file(GLOB LDC_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cpp/*.cpp")

set(LDC_TEST_IR "")
foreach(src IN LISTS LDC_TEST_SOURCES)
  get_filename_component(name "${src}" NAME_WE)
  ldc_compile_ir("${src}" ll)
  list(APPEND LDC_TEST_IR "${ll}")
  set(expect "${CMAKE_CURRENT_SOURCE_DIR}/expected/${name}.json")
  # M2: L_FC at k = 0 must equal SVF Andersen on every variable, unless the expectation
  # file says "andersen": false (and its note says why).
  file(READ "${expect}" expect_json)
  string(JSON compare_andersen ERROR_VARIABLE no_key GET "${expect_json}" andersen)
  if(no_key OR compare_andersen)
    add_test(NAME "${name}.andersen"
             COMMAND ldc -stat=false -ldc-andersen
                     "-ldc-src=${src}" "-ldc-expect=${expect}" "${ll}")
  endif()
  # M3, M4, phase 2: the expected queries, per context depth and analysis.
  foreach(k 0 1 2)
    foreach(mode lfc kcfa ldc ldcr)
      add_test(NAME "${name}.${mode}.k${k}"
               COMMAND ldc -stat=false -ldc-k=${k} -ldc-mode=${mode}
                       "-ldc-src=${src}" "-ldc-expect=${expect}" "${ll}")
    endforeach()
  endforeach()
endforeach()

add_custom_target(test_ir ALL DEPENDS ${LDC_TEST_IR})
