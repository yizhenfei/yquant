execute_process(COMMAND "${MONOD}" --funds
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics
  TIMEOUT 3)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "monod --funds failed: ${result}\n${diagnostics}")
endif()
foreach(event IN ITEMS
    "funds account=7 ready=false"
    "funds account=8 ready=false"
    "funds account=7 ready=true refresh=1 currency=CNY balance=1000 available=700 margin=200 frozen_margin=100"
    "funds account=8 ready=true refresh=1 currency=USD balance=0 available=0 margin=0 frozen_margin=0"
    "funds account=7 ready=true refresh=2 currency=CNY balance=900 available=650 margin=150 frozen_margin=100"
    "funds account=8 ready=true refresh=2 currency=USD balance=2000 available=1500 margin=400 frozen_margin=100"
    "funds_summary accounts=2 refreshes=2")
  string(FIND "${output}" "${event}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Missing ${event}:\n${output}")
  endif()
endforeach()
if(NOT diagnostics STREQUAL "")
  message(FATAL_ERROR "Unexpected funds diagnostics: ${diagnostics}")
endif()
