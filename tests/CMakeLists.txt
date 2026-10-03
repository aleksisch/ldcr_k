# Compile every tests/cpp/*.cpp to LLVM IR and register CTests that run `ldc` on it.

# ldc_compare(<name> <ll> <k> <mode> sound|same|p3ctx): runs kcfa and <mode> at depth k and
# compares their facts (-ldc-facts): `sound`, every kCFA fact is a fact of <mode>; `same`, equal
# facts; `p3ctx`, <mode> with -ldc-p3ctx has kCFA's facts once contexts are dropped.
function(ldc_compare name ll k mode relation)
  set(out "${CMAKE_CURRENT_BINARY_DIR}/${name}.k${k}.${mode}")
  set(run "$<TARGET_FILE:ldc> -ldc-k=${k}")
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
    add_test(NAME "${name}.andersen" COMMAND ldc -ldc-andersen "${ll}")
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
               COMMAND sh -c "$<TARGET_FILE:ldc> -ldc-k=${k} -ldc-mode=${mode} '-ldc-src=${src}' '-ldc-facts=${out}' '${ll}' >/dev/null && sh '${CMAKE_CURRENT_SOURCE_DIR}/expect.sh' '${out}' '${expect}.txt'")
    endforeach()
  endforeach()
endforeach()

add_custom_target(test_ir ALL DEPENDS ${LDC_TEST_IR})

# Shared LLVM/SVF infrastructure checks.
# Build the two input programs used by the driver tests.
ldc_compile_ir("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/call_graph.cpp" call_graph_ir)
ldc_compile_ir("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pointer_flow.cpp" pointer_flow_ir)
add_custom_target(frontend_ir ALL DEPENDS "${call_graph_ir}" "${pointer_flow_ir}")

# One shared runner, generated into the build directory for CTest.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/check_driver.cmake" CONTENT [[
if(FILE)
  file(REMOVE "${FILE}")
endif()
execute_process(COMMAND "${DRIVER}" ${ARGS}
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status STREQUAL "${STATUS}")
  message(FATAL_ERROR "Exit ${status}, expected ${STATUS}:\n${output}\n${error}")
endif()
if(output MATCHES "${REJECT}")
  message(FATAL_ERROR "Unexpected driver output:\n${output}")
endif()
if(STATUS STREQUAL "0")
  set(text "${output}")
else()
  set(text "${error}")
endif()
if(FILE)
  file(READ "${FILE}" text)
  file(REMOVE "${FILE}")
endif()
foreach(fragment IN LISTS EXPECT)
  string(FIND "${text}" "${fragment}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing '${fragment}':\n${text}")
  endif()
endforeach()
]])

function(driver_test name)
  cmake_parse_arguments(T "" "STATUS;FILE;REJECT" "ARGS;EXPECT" ${ARGN})
  if(NOT T_STATUS)
    set(T_STATUS 0)
  endif()
  set(reject "NumOfStores|SolveIterations|Persistent Points-To Cache Statistics|TotalUnions")
  if(T_REJECT)
    string(APPEND reject "|${T_REJECT}")
  endif()
  add_test(NAME "${name}" COMMAND "${CMAKE_COMMAND}" "-DDRIVER=$<TARGET_FILE:ldc>"
    "-DARGS=${T_ARGS}" "-DSTATUS=${T_STATUS}" "-DFILE=${T_FILE}"
    "-DEXPECT=${T_EXPECT}" "-DREJECT=${reject}"
    -P "${CMAKE_CURRENT_BINARY_DIR}/check_driver.cmake")
endfunction()

driver_test(call_graph ARGS "${call_graph_ir}"
  EXPECT "call: main -> direct\n" "call: main -> invoke\n" "call: invoke -> indirect\n")
set(dot "${CMAKE_CURRENT_BINARY_DIR}/program.dot")
driver_test(program_dot ARGS --program-dot "${dot}" "${pointer_flow_ir}" FILE "${dot}"
  EXPECT "digraph SimplifiedPAG" "store[2]" "load[2]" "gep[2]" "enter@" "exit@"
         "pointer_flow.cpp" "inspect(Box*, int*, bool)" "loaded")
driver_test(missing_input STATUS 1 EXPECT "no input files")
driver_test(program_dot.write_error STATUS 1
  ARGS "-program-dot=${CMAKE_CURRENT_BINARY_DIR}" "${pointer_flow_ir}"
  EXPECT "Cannot write program graph")

foreach(flag --help -h)
  driver_test("help.${flag}" ARGS "${flag}" EXPECT "Usage: ldc" "--program-dot"
    REJECT "SVF|  -ander|  -stat|help-svf")
endforeach()
foreach(flag -stat=false -dump-callgraph -ander --help-svf --unknown)
  driver_test("unknown.${flag}" ARGS "${flag}" STATUS 1 EXPECT "unknown option")
endforeach()
foreach(flag --program-dot --program-dot=)
  driver_test("missing_path.${flag}" ARGS "${flag}" STATUS 1 EXPECT "requires an output path")
endforeach()
