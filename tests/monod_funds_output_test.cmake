execute_process(COMMAND "${SCENARIOS}" --funds
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics
  TIMEOUT 3)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "yquant_scenarios --funds failed: ${result}\n${diagnostics}")
endif()
foreach(event IN ITEMS
    "funds account=7 ready=false"
    "funds account=8 ready=false"
    "funds account=7 ready=true refresh=1 currency=CNY balance=1000 available=800 margin=200 frozen_margin=0"
    "funds account=8 ready=true refresh=1 currency=USD balance=0 available=0 margin=0 frozen_margin=0"
    "funds_summary accounts=2 initial_syncs=1")
  string(FIND "${output}" "${event}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Missing ${event}:\n${output}")
  endif()
endforeach()
if(NOT diagnostics STREQUAL "")
  message(FATAL_ERROR "Unexpected funds diagnostics: ${diagnostics}")
endif()
