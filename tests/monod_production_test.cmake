# The production executable must never dispatch the migrated verification paths.
execute_process(COMMAND "${MONOD}" RESULT_VARIABLE rc
  OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 3)
if(NOT rc EQUAL 1 OR NOT out STREQUAL "" OR NOT err MATCHES "configuration")
  message(FATAL_ERROR "Default production startup did not report missing runtime support")
endif()
execute_process(COMMAND "${MONOD}" --help RESULT_VARIABLE rc
  OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 3)
if(NOT rc EQUAL 0 OR NOT err STREQUAL "" OR out MATCHES "maker|simnow|orders")
  message(FATAL_ERROR "Production help exposed verification commands")
endif()
foreach(flag IN ITEMS --maker --orders --cancel --fills --funds --positions
    --reserve --startup --funds-lifecycle --positions-lifecycle
    --simnow-market --simnow-trader-login --simnow-startup-snapshot)
  execute_process(COMMAND "${MONOD}" "${flag}" RESULT_VARIABLE rc
    OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 3)
  if(NOT rc EQUAL 2 OR NOT out STREQUAL "" OR NOT err MATCHES "Unsupported argument")
    message(FATAL_ERROR "Production executable accepted ${flag}")
  endif()
endforeach()
