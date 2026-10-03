# Build the LLVM IR fixtures as part of the default build.
ldc_compile_ir("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/call_graph.cpp" call_graph_ir)
add_custom_target(test_ir ALL DEPENDS "${call_graph_ir}")

# Run the driver and check its resolved call edges.
# CTest runs this generated script at test time, after the driver is built.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/check_call_graph.cmake" CONTENT [[
execute_process(COMMAND "${DRIVER}" "${IR}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Driver failed: ${status}\n${output}\n${error}")
endif()
if(output MATCHES "NumOfStores|SolveIterations|Persistent Points-To Cache Statistics|TotalUnions")
  message(FATAL_ERROR "Dependency statistics leaked into output:
${output}")
endif()
foreach(edge "main -> direct" "main -> invoke" "invoke -> indirect")
  string(FIND "${output}" "call: ${edge}\n" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing call edge ${edge}:\n${output}")
  endif()
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
add_test(NAME frontend COMMAND frontend_test "${pointer_flow_ir}")
add_test(NAME frontend.no-debug COMMAND frontend_test "${pointer_flow_no_debug}")

# Check the exported simplified PAG, including fields, calls, and source metadata.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/check_program_graph.cmake" CONTENT [[
execute_process(COMMAND "${DRIVER}" --program-dot "${DOT}" "${IR}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Graph export failed: ${status}\n${output}\n${error}")
endif()
if(output MATCHES "NumOfStores|SolveIterations|Persistent Points-To Cache Statistics|TotalUnions")
  message(FATAL_ERROR "Dependency statistics leaked into export output:
${output}")
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
add_test(NAME program_dot.write_error COMMAND ldc
  "-program-dot=${CMAKE_CURRENT_BINARY_DIR}" "${pointer_flow_ir}")
set_tests_properties(program_dot.write_error PROPERTIES WILL_FAIL TRUE)


# Help and invalid options are handled by our CLI before any analysis starts.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/check_cli.cmake" CONTENT [[
foreach(flag --help -h)
  execute_process(COMMAND "${DRIVER}" "${flag}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT status STREQUAL "0" OR NOT error STREQUAL "")
    message(FATAL_ERROR "Help failed: ${status}
${output}
${error}")
  endif()
  if(NOT output MATCHES "Usage: ldc" OR NOT output MATCHES "--program-dot")
    message(FATAL_ERROR "Missing tool help: ${output}")
  endif()
  if(output MATCHES "SVF|  -ander|  -stat|help-svf")
    message(FATAL_ERROR "Dependency options leaked into help: ${output}")
  endif()
endforeach()
foreach(flag -stat=false -dump-callgraph -ander --help-svf --unknown)
  execute_process(COMMAND "${DRIVER}" "${flag}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT status STREQUAL "1" OR NOT output STREQUAL "" OR NOT error MATCHES "unknown option")
    message(FATAL_ERROR "Unsupported option was not rejected: ${flag}
${output}
${error}")
  endif()
endforeach()
foreach(flag --program-dot --program-dot=)
  execute_process(COMMAND "${DRIVER}" "${flag}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT status STREQUAL "1" OR NOT error MATCHES "requires an output path")
    message(FATAL_ERROR "Missing output path was not rejected: ${output}
${error}")
  endif()
endforeach()
]])
add_test(NAME cli
  COMMAND "${CMAKE_COMMAND}" "-DDRIVER=$<TARGET_FILE:ldc>"
          -P "${CMAKE_CURRENT_BINARY_DIR}/check_cli.cmake")
