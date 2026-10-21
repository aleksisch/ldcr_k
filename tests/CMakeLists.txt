ldc_compile_ir("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/call_graph.cpp" call_graph_ir)
ldc_compile_ir("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pointer_flow.cpp" pointer_flow_ir)
ldc_compile_ir("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dispatch.cpp" dispatch_ir)
add_custom_target(test_ir ALL DEPENDS "${call_graph_ir}" "${pointer_flow_ir}" "${dispatch_ir}")

add_executable(cfl_test cfl_test.cpp)
target_link_libraries(cfl_test PRIVATE ldc_cfl)
add_test(NAME cfl COMMAND cfl_test)
add_executable(grammar_test grammar_test.cpp)
target_link_libraries(grammar_test PRIVATE ldc_cfl)
add_test(NAME grammar COMMAND grammar_test)

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

# Points-to facts of every tests/cpp/*.cpp against expected/<name>.txt.
file(GLOB points_to_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cpp/*.cpp")
set(points_to_ir "")
foreach(src IN LISTS points_to_sources)
  get_filename_component(name "${src}" NAME_WE)
  ldc_compile_ir("${src}" ll)
  list(APPEND points_to_ir "${ll}")
  set(facts "${CMAKE_CURRENT_BINARY_DIR}/${name}.facts")
  add_test(NAME "points_to.${name}"
           COMMAND sh -c "$<TARGET_FILE:ldc> --facts '${facts}' --source '${src}' '${ll}' >/dev/null && sh '${CMAKE_CURRENT_SOURCE_DIR}/expect.sh' '${facts}' '${CMAKE_CURRENT_SOURCE_DIR}/expected/${name}.txt'")
  # With calls and returns matched (L_C), where expected/<name>.calls.txt exists.
  set(calls "${CMAKE_CURRENT_SOURCE_DIR}/expected/${name}.calls.txt")
  if(EXISTS "${calls}")
    set(facts "${CMAKE_CURRENT_BINARY_DIR}/${name}.calls.facts")
    add_test(NAME "points_to.${name}.calls"
             COMMAND sh -c "$<TARGET_FILE:ldc> --match-calls --facts '${facts}' --source '${src}' '${ll}' >/dev/null && sh '${CMAKE_CURRENT_SOURCE_DIR}/expect.sh' '${facts}' '${calls}'")
  endif()
  # With the C++ heap model (--cpp-heap), against the same expected facts.
  set(facts "${CMAKE_CURRENT_BINARY_DIR}/${name}.cpp_heap.facts")
  add_test(NAME "points_to.${name}.cpp_heap"
           COMMAND sh -c "$<TARGET_FILE:ldc> --cpp-heap --facts '${facts}' --source '${src}' '${ll}' >/dev/null && sh '${CMAKE_CURRENT_SOURCE_DIR}/expect.sh' '${facts}' '${CMAKE_CURRENT_SOURCE_DIR}/expected/${name}.txt'")
endforeach()
add_custom_target(points_to_ir ALL DEPENDS ${points_to_ir})
