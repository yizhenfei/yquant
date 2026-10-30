execute_process(COMMAND "${MONOD}" --cancel
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "monod --cancel failed: ${result}\n${diagnostics}")
endif()
foreach(event IN ITEMS
    "order caller=100 sequence=1 account=7 strategy=2 state=canceled"
    "cancel sequence=1 request=1 result=canceled"
    "cancel sequence=4 request=1 result=rejected"
    "cancel_summary reports=2 canceled=1"
    "trading_summary reports=4 failures=2")
  string(FIND "${output}" "${event}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Missing ${event}:\n${output}")
  endif()
endforeach()
if(diagnostics MATCHES "result=rejected")
  message(FATAL_ERROR "Counter cancellation rejection must go to stdout")
endif()
