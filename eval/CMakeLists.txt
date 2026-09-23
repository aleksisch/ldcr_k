# M5: evaluation programs. Each is compiled to IR; CTest checks soundness against kCFA
# and L_DCR_k = kCFA (`<name>.eval.k<k>`), and the `eval` target writes the comparison tables to
# ${CMAKE_CURRENT_BINARY_DIR}/results.md.

file(GLOB LDC_EVAL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cpp/*.cpp")

set(LDC_EVAL_IR "")
set(LDC_EVAL_COMMANDS "")
set(results "${CMAKE_CURRENT_BINARY_DIR}/results.md")
foreach(src IN LISTS LDC_EVAL_SOURCES)
  get_filename_component(name "${src}" NAME_WE)
  ldc_compile_ir("${src}" ll)
  list(APPEND LDC_EVAL_IR "${ll}")
  list(APPEND LDC_EVAL_COMMANDS COMMAND ${CMAKE_COMMAND} -E echo "## ${name}" >> "${results}")
  foreach(k 0 1 2)
    add_test(NAME "${name}.eval.k${k}" COMMAND ldc -stat=false -ldc-eval -ldc-k=${k} "${ll}")
    list(APPEND LDC_EVAL_COMMANDS
         COMMAND sh -c "$<TARGET_FILE:ldc> -stat=false -ldc-eval -ldc-k=${k} '${ll}' 2>/dev/null | sed -n '/^k = /,/^Failures/p' >> '${results}'")
  endforeach()
endforeach()

add_custom_target(eval_ir ALL DEPENDS ${LDC_EVAL_IR})
add_custom_target(eval
  COMMAND ${CMAKE_COMMAND} -E rm -f "${results}"
  ${LDC_EVAL_COMMANDS}
  DEPENDS ldc ${LDC_EVAL_IR}
  COMMENT "Evaluation tables -> ${results}"
  VERBATIM)
