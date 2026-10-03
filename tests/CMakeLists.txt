ldc_compile_ir("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/call_graph.cpp" call_graph_ir)
add_custom_target(test_ir ALL DEPENDS "${call_graph_ir}")

add_test(NAME call_graph
  COMMAND "${CMAKE_COMMAND}" "-DDRIVER=$<TARGET_FILE:ldc>"
          "-DIR=${call_graph_ir}" -P "${CMAKE_CURRENT_SOURCE_DIR}/check_call_graph.cmake")
add_test(NAME missing_input COMMAND ldc)
set_tests_properties(missing_input PROPERTIES WILL_FAIL TRUE)
