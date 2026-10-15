foreach(required CXX_COMPILER PROJECT_INCLUDE FMT_INCLUDE SOURCE OUTPUT
                 COMPILE_CASE EXPECT_SUCCESS)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Missing compile-check argument: ${required}")
  endif()
endforeach()

set(compiler_args)
if(CXX_COMPILER_ARG1)
  separate_arguments(extra_args NATIVE_COMMAND "${CXX_COMPILER_ARG1}")
  list(APPEND compiler_args ${extra_args})
endif()
if(CXX_FLAGS)
  separate_arguments(extra_args NATIVE_COMMAND "${CXX_FLAGS}")
  list(APPEND compiler_args ${extra_args})
endif()
if(OSX_SYSROOT)
  list(APPEND compiler_args -isysroot "${OSX_SYSROOT}")
endif()
foreach(architecture IN LISTS OSX_ARCHITECTURES)
  list(APPEND compiler_args -arch "${architecture}")
endforeach()
list(APPEND compiler_args
  -std=c++20 -fno-exceptions -O0
  -DFMT_USE_EXCEPTIONS=0
  -DYQ_LOG_COMPILED_LEVEL=YQ_LOG_LEVEL_INFO
  "-DYQUANT_LOG_COMPILE_CASE=${COMPILE_CASE}"
  "-I${PROJECT_INCLUDE}" "-I${FMT_INCLUDE}"
  -c "${SOURCE}" -o "${OUTPUT}")

get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(REMOVE "${OUTPUT}")
execute_process(COMMAND "${CXX_COMPILER}" ${compiler_args}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE standard_output
  ERROR_VARIABLE standard_error
  TIMEOUT 45)

if(EXPECT_SUCCESS)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR
      "Expected successful compilation; result=${result}\n${standard_output}${standard_error}")
  endif()
  if(COMPILE_CASE EQUAL 0)
    file(STRINGS "${OUTPUT}" retained_strings REGEX "retained-info=")
    if(NOT retained_strings)
      message(FATAL_ERROR "Retained INFO log is absent from the object")
    endif()
    file(STRINGS "${OUTPUT}" disabled_strings
      REGEX "YQ_DISABLED_(DEBUG|TRACE)_CALL_SITE_ONLY_3b43")
    if(disabled_strings)
      message(FATAL_ERROR "Discarded log strings remain in object: ${disabled_strings}")
    endif()
  endif()
elseif(result STREQUAL "0")
  message(FATAL_ERROR "Invalid logging call compiled successfully")
elseif(NOT result MATCHES "^[0-9]+$")
  message(FATAL_ERROR
    "Compiler did not complete; result=${result}\n${standard_output}${standard_error}")
endif()
