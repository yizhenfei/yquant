execute_process(COMMAND "${MONOD}" --fills
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics
  TIMEOUT 3)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "monod --fills failed: ${result}\n${diagnostics}")
endif()
foreach(event IN ITEMS
    "trade sequence=1 id=sim/day-1/1 price=100.25 quantity=1 state=partially_filled filled=1"
    "trade sequence=4 id=sim/day-1/1 price=100.5 quantity=3 state=filled filled=3"
    "trade sequence=1 id=sim/day-1/2 price=100.5 quantity=2 state=filled filled=3"
    "query sequence=1 account=7 state=filled filled=3"
    "query sequence=4 account=8 state=filled filled=3"
    "fill_summary trades=3"
    "trading_summary reports=3 failures=2")
  string(FIND "${output}" "${event}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Missing ${event}:\n${output}")
  endif()
endforeach()
if(diagnostics MATCHES "trade sequence=|query sequence=|fill_summary")
  message(FATAL_ERROR "Business fill and query output must go to stdout")
endif()
