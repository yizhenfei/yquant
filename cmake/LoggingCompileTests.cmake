# Compile independently at -O0 so discarded calls cannot rely on optimization.
foreach(compile_case RANGE 0 7)
  set(expect_success OFF)
  if(compile_case EQUAL 0 OR compile_case EQUAL 7)
    set(expect_success ON)
  endif()
  add_test(NAME LoggingCompile.Case${compile_case}
    COMMAND "${CMAKE_COMMAND}"
      "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DCXX_COMPILER_ARG1=${CMAKE_CXX_COMPILER_ARG1}"
      "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
      "-DOSX_SYSROOT=${CMAKE_OSX_SYSROOT}"
      "-DOSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
      "-DPROJECT_INCLUDE=${PROJECT_SOURCE_DIR}/include"
      "-DFMT_INCLUDE=${fmt_SOURCE_DIR}/include"
      "-DSOURCE=${PROJECT_SOURCE_DIR}/tests/compile/logging_compile_check.cc"
      "-DOUTPUT=${PROJECT_BINARY_DIR}/compile_checks/$<CONFIG>/logging_${compile_case}.o"
      "-DCOMPILE_CASE=${compile_case}"
      "-DEXPECT_SUCCESS=${expect_success}"
      -P "${PROJECT_SOURCE_DIR}/cmake/CheckLoggingCompile.cmake")
  set_tests_properties(LoggingCompile.Case${compile_case}
    PROPERTIES ${yquant_test_properties})
endforeach()
