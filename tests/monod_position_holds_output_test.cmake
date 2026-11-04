execute_process(COMMAND "${SCENARIOS}" --positions-lifecycle
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics
  TIMEOUT 3)
set(expected [=[position_close stage=submitted quantity=10 frozen=7 available=3
position_close stage=partial quantity=7 frozen=4 available=3
position_close stage=canceled quantity=7 frozen=0 available=7
position_close stage=reused quantity=7 frozen=7 available=0
]=])
if(NOT result EQUAL 0 OR NOT output STREQUAL expected OR NOT diagnostics STREQUAL "")
  message(FATAL_ERROR "Position holds scenario: ${result}\n${output}\n${diagnostics}")
endif()
