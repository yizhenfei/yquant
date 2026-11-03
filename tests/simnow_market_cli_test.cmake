execute_process(COMMAND "${FIXTURE}" --simnow-market tcp://fixture:1 IFtest 0
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 3)
if(NOT rc EQUAL 2 OR NOT out STREQUAL "" OR NOT err MATCHES "Usage:")
  message(FATAL_ERROR "Invalid count did not produce a usage error")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=YQUANT_CTP_PASSWORD
  "${MONOD}" --simnow-market tcp://fixture:1 IFtest
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 3)
if(NOT rc EQUAL 2 OR NOT err MATCHES "YQUANT_CTP_PASSWORD")
  message(FATAL_ERROR "Missing credentials did not prevent connection")
endif()
execute_process(COMMAND "${FIXTURE}" --simnow-market tcp://stalled:1 IFtest 1 1
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 4)
if(NOT rc EQUAL 1 OR NOT out STREQUAL "" OR NOT err MATCHES "timeout")
  message(FATAL_ERROR "Unanswered connection did not terminate at deadline")
endif()
