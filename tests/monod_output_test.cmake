execute_process(COMMAND "${MONOD}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
  TIMEOUT 3)
if(NOT result STREQUAL "0")
  message(FATAL_ERROR "monod failed: ${result}: ${error}")
endif()
set(expected "quote_intent instrument=42 bid=100.25 ask=100.5\n")
if(NOT output STREQUAL expected OR NOT error STREQUAL "")
  message(FATAL_ERROR "Unexpected monod output: stdout=[${output}] stderr=[${error}]")
endif()
