ldc_compile_ir("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/call_graph.cpp" call_graph_ir)
ldc_compile_ir("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pointer_flow.cpp" pointer_flow_ir)
ldc_compile_ir("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dispatch.cpp" dispatch_ir)
add_custom_target(test_ir ALL DEPENDS "${call_graph_ir}" "${pointer_flow_ir}" "${dispatch_ir}")

add_executable(dispatch_test dispatch_test.cpp)
target_link_libraries(dispatch_test PRIVATE ldc_frontend)
add_dependencies(dispatch_test test_ir)
add_test(NAME dispatch_graph COMMAND dispatch_test "${dispatch_ir}")

add_test(NAME call_graph COMMAND ldc "${call_graph_ir}")
add_test(NAME program_dot COMMAND ldc --program-dot
  "${CMAKE_CURRENT_BINARY_DIR}/program.dot" "${pointer_flow_ir}")
add_test(NAME program_dot.write_error COMMAND ldc --program-dot
  "${CMAKE_CURRENT_BINARY_DIR}" "${pointer_flow_ir}")
add_test(NAME missing_input COMMAND ldc)
set_tests_properties(program_dot.write_error missing_input PROPERTIES WILL_FAIL TRUE)

foreach(flag --help -h)
  add_test(NAME "help.${flag}" COMMAND ldc "${flag}")
endforeach()

foreach(flag -stat=false -dump-callgraph -ander --help-svf --unknown --program-dot --program-dot=)
  add_test(NAME "invalid.${flag}" COMMAND ldc "${flag}")
  set_tests_properties("invalid.${flag}" PROPERTIES WILL_FAIL TRUE)
endforeach()
