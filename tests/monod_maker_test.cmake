set(expected [=[maker_submit id=1 side=buy price=100
maker_submit id=2 side=sell price=102
maker_cancel id=1
maker_submit id=3 side=buy price=101
maker_summary quotes=1280 submits=1280 cancels=1278 bid=100 ask=102
]=])
foreach(run RANGE 1 2)
  execute_process(COMMAND "${MONOD}" --maker RESULT_VARIABLE result
    OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 5)
  set(stats "warmup=256 samples=1024 p50=[0-9]+ p95=[0-9]+ p99=[0-9]+")
  string(REGEX MATCH "maker_latency_ns phase=cancel ${stats}\nmaker_latency_ns phase=replace ${stats}\n$" timing "${out}")
  string(REPLACE "${timing}" "" trace "${out}")
  if(NOT result EQUAL 0 OR NOT err STREQUAL "" OR timing STREQUAL "" OR
     NOT trace STREQUAL expected)
    message(FATAL_ERROR "maker run ${run}: ${result}\n${out}\n${err}")
  endif()
endforeach()
