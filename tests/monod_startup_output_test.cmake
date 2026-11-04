execute_process(COMMAND "${SCENARIOS}" --startup
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics
  TIMEOUT 3)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "yquant_scenarios --startup failed: ${result}\n${diagnostics}")
endif()
foreach(event IN ITEMS
    "startup account=7 ready=false attempts=1"
    "startup account=7 ready=true attempts=2 funds=90 positions=1"
    "startup_summary late_snapshot_ignored=true")
  string(FIND "${output}" "${event}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Missing ${event}:\n${output}")
  endif()
endforeach()
if(NOT diagnostics MATCHES "initial_sync_failed account=7 sync=1")
  message(FATAL_ERROR "Missing startup failure diagnostic: ${diagnostics}")
endif()
if(output MATCHES "initial_sync_failed")
  message(FATAL_ERROR "Diagnostic written to stdout: ${output}")
endif()
