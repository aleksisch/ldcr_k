ldc_compile_ir("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/call_graph.cpp" call_graph_ir)
add_custom_target(test_ir ALL DEPENDS "${call_graph_ir}")

add_test(NAME call_graph
  COMMAND "${CMAKE_COMMAND}" "-DDRIVER=$<TARGET_FILE:ldc>"
          "-DIR=${call_graph_ir}" -P "${CMAKE_CURRENT_SOURCE_DIR}/check_call_graph.cmake")
add_test(NAME missing_input COMMAND ldc)
set_tests_properties(missing_input PROPERTIES WILL_FAIL TRUE)

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
add_test(NAME program_dot
  COMMAND "${CMAKE_COMMAND}" "-DDRIVER=$<TARGET_FILE:ldc>" "-DIR=${pointer_flow_ir}"
          "-DDOT=${CMAKE_CURRENT_BINARY_DIR}/program.dot"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/check_program_graph.cmake")
add_test(NAME program_dot.write_error COMMAND ldc -stat=false
  "-program-dot=${CMAKE_CURRENT_BINARY_DIR}" "${pointer_flow_ir}")
set_tests_properties(program_dot.write_error PROPERTIES WILL_FAIL TRUE)
