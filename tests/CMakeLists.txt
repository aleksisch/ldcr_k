# Compile every tests/cpp/*.cpp to LLVM IR and register CTests that run `ldc` on it.

# ldc_compare(<name> <ll> <k> <mode> sound|same|p3ctx): runs kcfa and <mode> at depth k and
# compares their facts (-ldc-facts): `sound`, every kCFA fact is a fact of <mode>; `same`, equal
# facts; `p3ctx`, <mode> with -ldc-p3ctx has kCFA's facts once contexts are dropped.
function(ldc_compare name ll k mode relation)
  set(out "${CMAKE_CURRENT_BINARY_DIR}/${name}.k${k}.${mode}")
  set(run "$<TARGET_FILE:ldc> -stat=false -ldc-k=${k}")
  set(flags "")
  if(relation STREQUAL "same")
    set(check "cmp '${out}.kcfa' '${out}.facts'")
  elseif(relation STREQUAL "p3ctx")
    set(out "${out}.p3ctx")
    set(flags "-ldc-p3ctx")
    set(drop "sed 's/\\[ [^]]*\\]//g'")
    set(check "${drop} '${out}.kcfa' | LC_ALL=C sort -u > '${out}.ci' && ${drop} '${out}.facts' | LC_ALL=C sort -u | cmp '${out}.ci' -")
  else()
    set(check "test -z \"$(LC_ALL=C comm -23 '${out}.kcfa' '${out}.facts')\"")
  endif()
  add_test(NAME "${name}.${mode}-${relation}.k${k}"
           COMMAND sh -c "${run} -ldc-mode=kcfa -ldc-facts='${out}.kcfa' '${ll}' >/dev/null && ${run} -ldc-mode=${mode} ${flags} -ldc-facts='${out}.facts' '${ll}' >/dev/null && ${check}")
endfunction()

file(GLOB LDC_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cpp/*.cpp")

set(LDC_TEST_IR "")
foreach(src IN LISTS LDC_TEST_SOURCES)
  get_filename_component(name "${src}" NAME_WE)
  ldc_compile_ir("${src}" ll)
  list(APPEND LDC_TEST_IR "${ll}")
  # Unless its expected files say `# andersen: false` (and why), SVF Andersen resolves this
  # program's calls, which L_FC is wired from.
  file(GLOB expected "${CMAKE_CURRENT_SOURCE_DIR}/expected/${name}.k*.txt")
  set(trust_andersen TRUE)
  foreach(file IN LISTS expected)
    file(STRINGS "${file}" no_andersen REGEX "^# andersen: false")
    if(no_andersen)
      set(trust_andersen FALSE)
    endif()
  endforeach()
  foreach(k 0 1 2)
    # L_DC_k keeps every kCFA fact, and so does L_FC_k where SVF resolves the calls.
    ldc_compare("${name}" "${ll}" ${k} ldc sound)
    if(trust_andersen)
      ldc_compare("${name}" "${ll}" ${k} lfc sound)
    endif()
    # L_DCR_k = kCFA, fact by fact.
    ldc_compare("${name}" "${ll}" ${k} ldcr same)
    # P3Ctx: contexts only where they can matter, same facts (paper Theorem 7).
    ldc_compare("${name}" "${ll}" ${k} kcfa p3ctx)
    ldc_compare("${name}" "${ll}" ${k} ldcr p3ctx)
  endforeach()
  if(NOT expected)
    continue() # the evaluation programs have no hand-written expectations
  endif()
  # L_FC at k = 0 equals SVF Andersen on every variable.
  if(trust_andersen)
    add_test(NAME "${name}.andersen" COMMAND ldc -stat=false -ldc-andersen "${ll}")
  endif()
  # The expected facts, per context depth and analysis (expected/README.md).
  foreach(k 0 1 2)
    foreach(mode lfc kcfa ldc ldcr)
      set(expect "${CMAKE_CURRENT_SOURCE_DIR}/expected/${name}.k${k}")
      if(EXISTS "${expect}.${mode}.txt")
        set(expect "${expect}.${mode}")
      elseif(NOT EXISTS "${expect}.txt")
        continue()
      endif()
      set(out "${CMAKE_CURRENT_BINARY_DIR}/${name}.${mode}.k${k}.facts")
      add_test(NAME "${name}.${mode}.k${k}"
               COMMAND sh -c "$<TARGET_FILE:ldc> -stat=false -ldc-k=${k} -ldc-mode=${mode} '-ldc-src=${src}' '-ldc-facts=${out}' '${ll}' >/dev/null && sh '${CMAKE_CURRENT_SOURCE_DIR}/expect.sh' '${out}' '${expect}.txt'")
    endforeach()
  endforeach()
endforeach()

add_custom_target(test_ir ALL DEPENDS ${LDC_TEST_IR})

# Shared LLVM/SVF infrastructure checks.
# Build the LLVM IR fixtures as part of the default build.
ldc_compile_ir("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/call_graph.cpp" call_graph_ir)
add_custom_target(call_graph_ir ALL DEPENDS "${call_graph_ir}")

# Run the driver and check both its call edges and SVF DOT exports.
# CTest runs this generated script at test time, after the driver is built.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/check_call_graph.cmake" CONTENT [[
execute_process(COMMAND "${DRIVER}" -stat=false -dump-callgraph "${IR}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Driver failed: ${status}\n${output}\n${error}")
endif()
foreach(edge "main -> direct" "main -> invoke" "invoke -> indirect")
  string(FIND "${output}" "call: ${edge}\n" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing call edge ${edge}:\n${output}")
  endif()
endforeach()
foreach(graph callgraph_initial.dot callgraph_final.dot)
  if(NOT EXISTS "${graph}")
    message(FATAL_ERROR "Missing SVF graph export: ${graph}")
  endif()
  file(SIZE "${graph}" size)
  if(size EQUAL 0)
    message(FATAL_ERROR "Empty SVF graph export: ${graph}")
  endif()
  file(REMOVE "${graph}")
endforeach()
]])
add_test(NAME call_graph
  COMMAND "${CMAKE_COMMAND}" "-DDRIVER=$<TARGET_FILE:ldc>"
          "-DIR=${call_graph_ir}" -P "${CMAKE_CURRENT_BINARY_DIR}/check_call_graph.cmake")

# These error cases must return a nonzero exit status.
add_test(NAME missing_input COMMAND ldc)
set_tests_properties(missing_input PROPERTIES WILL_FAIL TRUE)

# Exercise the frontend with and without LLVM debug information.
ldc_compile_ir("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pointer_flow.cpp" pointer_flow_ir)
set(pointer_flow_no_debug "${CMAKE_CURRENT_BINARY_DIR}/pointer_flow.no-debug.ll")
add_custom_command(OUTPUT "${pointer_flow_no_debug}"
  COMMAND "${LDC_OPT}" -S -strip-debug "${pointer_flow_ir}" -o "${pointer_flow_no_debug}"
  DEPENDS "${pointer_flow_ir}" VERBATIM)
add_custom_target(frontend_ir ALL DEPENDS "${pointer_flow_ir}" "${pointer_flow_no_debug}")
add_executable(frontend_test frontend_test.cpp)
target_link_libraries(frontend_test PRIVATE ldc_frontend)
add_dependencies(frontend_test frontend_ir)
add_test(NAME frontend COMMAND frontend_test -stat=false "${pointer_flow_ir}")
add_test(NAME frontend.no-debug COMMAND frontend_test -stat=false "${pointer_flow_no_debug}")

# Check the exported simplified PAG, including fields, calls, and source metadata.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/check_program_graph.cmake" CONTENT [[
execute_process(COMMAND "${DRIVER}" -stat=false "-program-dot=${DOT}" "${IR}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Graph export failed: ${status}\n${output}\n${error}")
endif()
file(READ "${DOT}" graph)
foreach(fragment "digraph SimplifiedPAG" "store[2]" "load[2]" "gep[2]"
                 "enter@" "exit@" "pointer_flow.cpp" "inspect(Box*, int*, bool)" "loaded")
  string(FIND "${graph}" "${fragment}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing graph content: ${fragment}")
  endif()
endforeach()
file(REMOVE "${DOT}")
]])
add_test(NAME program_dot
  COMMAND "${CMAKE_COMMAND}" "-DDRIVER=$<TARGET_FILE:ldc>" "-DIR=${pointer_flow_ir}"
          "-DDOT=${CMAKE_CURRENT_BINARY_DIR}/program.dot"
          -P "${CMAKE_CURRENT_BINARY_DIR}/check_program_graph.cmake")

# A directory cannot be opened as a DOT output file.
add_test(NAME program_dot.write_error COMMAND ldc -stat=false
  "-program-dot=${CMAKE_CURRENT_BINARY_DIR}" "${pointer_flow_ir}")
set_tests_properties(program_dot.write_error PROPERTIES WILL_FAIL TRUE)
