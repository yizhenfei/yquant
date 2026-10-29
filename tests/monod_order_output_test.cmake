execute_process(COMMAND "${MONOD}" --orders
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "monod --orders failed: ${result}\n${diagnostics}")
endif()
set(expected
  "order caller=100 sequence=1 account=7 strategy=2 state=accepted\n"
  "order caller=100 sequence=2 account=8 strategy=2 state=rejected\n"
  "order caller=100 sequence=4 account=8 strategy=2 state=accepted\n"
  "trading_summary reports=3 failures=2\n")
string(JOIN "" expected ${expected})
if(NOT output STREQUAL expected)
  message(FATAL_ERROR "Unexpected business output:\n${output}")
endif()
foreach(reason IN ITEMS send_failed)
  if(NOT diagnostics MATCHES "request_failure [^\n]*reason=${reason}")
    message(FATAL_ERROR "Missing ${reason} diagnostic:\n${diagnostics}")
  endif()
endforeach()
if(NOT diagnostics MATCHES "report_after_send_failure")
  message(FATAL_ERROR "Missing late report diagnostic:\n${diagnostics}")
endif()
if(diagnostics MATCHES "state=rejected")
  message(FATAL_ERROR "Counter rejection must go to stdout")
endif()
execute_process(COMMAND "${MONOD}" --help
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics)
if(NOT result EQUAL 0 OR NOT output MATCHES "Usage:" OR NOT diagnostics STREQUAL "")
  message(FATAL_ERROR "Help must succeed on stdout")
endif()
execute_process(COMMAND "${MONOD}" --invalid
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics)
if(result EQUAL 0 OR NOT output STREQUAL "" OR NOT diagnostics MATCHES "Usage:")
  message(FATAL_ERROR "Invalid arguments must fail on stderr")
endif()
