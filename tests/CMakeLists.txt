# Build the LLVM IR fixtures as part of the default build.
ldc_compile_ir("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/call_graph.cpp" call_graph_ir)
add_custom_target(test_ir ALL DEPENDS "${call_graph_ir}")

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
