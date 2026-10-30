execute_process(COMMAND "${MONOD}" --positions
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics
  TIMEOUT 3)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "monod --positions failed: ${result}\n${diagnostics}")
endif()
foreach(event IN ITEMS
    "positions account=7 ready=false"
    "positions account=8 ready=false"
    "positions account=7 ready=true refresh=1 count=2"
    "position account=7 instrument=42 side=long hedge=speculation today=3 yesterday=7 today_frozen=1 yesterday_frozen=2"
    "positions account=8 ready=true refresh=1 count=1"
    "position account=8 instrument=42 side=long hedge=speculation today=20 yesterday=30 today_frozen=4 yesterday_frozen=5"
    "positions account=7 ready=true refresh=2 count=2"
    "position account=7 instrument=42 side=long hedge=speculation today=5 yesterday=2 today_frozen=2 yesterday_frozen=1"
    "positions account=7 ready=true refresh=3 count=0"
    "zero_position account=7 instrument=42 today=0 yesterday=0"
    "positions_summary accounts=2 refreshes=3")
  string(FIND "${output}" "${event}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Missing ${event}:\n${output}")
  endif()
endforeach()
if(NOT diagnostics STREQUAL "")
  message(FATAL_ERROR "Unexpected positions diagnostics: ${diagnostics}")
endif()
