execute_process(COMMAND "${MONOD}" --reserve
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics
  TIMEOUT 3)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "monod --reserve failed: ${result}\n${diagnostics}")
endif()
foreach(event IN ITEMS
    "reservation account=7 reserved=70 available_for_orders=30"
    "reservation_failure sequence=2 reason=insufficient_funds"
    "reservation account=7 reserved=91 available_for_orders=9"
    "reservation_summary sent=2 insufficient=1 send_failed=1")
  string(FIND "${output}" "${event}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Missing ${event}:\n${output}")
  endif()
endforeach()
if(NOT diagnostics MATCHES "reservation_failure sequence=3 reason=send_failed")
  message(FATAL_ERROR "Missing expected send failure: ${diagnostics}")
endif()
if(diagnostics MATCHES "reason=insufficient_funds")
  message(FATAL_ERROR "Expected business rejection belongs on stdout")
endif()
