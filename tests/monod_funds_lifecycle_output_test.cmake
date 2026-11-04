execute_process(COMMAND "${SCENARIOS}" --funds-lifecycle
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics
  TIMEOUT 3)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "yquant_scenarios --funds-lifecycle failed: ${result}\n${diagnostics}")
endif()
foreach(event IN ITEMS
    "stage=submitted available=978 reserved=22 margin=0 commission=0 realized_pnl=0"
    "stage=partial available=979 reserved=11 margin=9 commission=1 realized_pnl=0"
    "stage=canceled available=990 reserved=0 margin=9 commission=1 realized_pnl=0"
    "stage=closed available=1018 reserved=0 margin=0 commission=2 realized_pnl=20"
    "funds_lifecycle_summary trades=2")
  string(FIND "${output}" "${event}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Missing ${event}:\n${output}")
  endif()
endforeach()
if(NOT diagnostics STREQUAL "")
  message(FATAL_ERROR "Unexpected diagnostics: ${diagnostics}")
endif()
