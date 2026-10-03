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
