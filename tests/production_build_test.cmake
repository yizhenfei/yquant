# A fresh configuration proves BUILD_TESTING=OFF also excludes test targets,
# GoogleTest setup, and all test/example translation units.
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${BINARY}"
  -G "${GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
  "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DYQUANT_CTP_SDK_ROOT=${SDK}"
  -DBUILD_TESTING=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "Production configuration failed: ${out}\n${err}")
endif()
file(READ "${BINARY}/CMakeFiles/TargetDirectories.txt" targets)
if(targets MATCHES "/(verification_|verify_simnow|simnow_.*(test|probe|fixture)|yquant_scenarios|[^/]*_test|gtest)")
  message(FATAL_ERROR "Test targets exist with BUILD_TESTING=OFF: ${targets}")
endif()
if(EXISTS "${BINARY}/compile_commands.json")
  file(READ "${BINARY}/compile_commands.json" commands)
  if(commands MATCHES "/(tests|examples)/")
    message(FATAL_ERROR "Production build includes verification sources")
  endif()
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BINARY}" --target monod --config Debug
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "Standalone production build failed: ${out}\n${err}")
endif()
find_program(production_monod monod PATHS "${BINARY}" "${BINARY}/Debug" NO_DEFAULT_PATH)
if(NOT production_monod)
  message(FATAL_ERROR "Production executable missing")
endif()
set(MONOD "${production_monod}")
include("${SOURCE}/tests/monod_production_test.cmake")
