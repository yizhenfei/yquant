set(YQUANT_TEST_DATA_DIR "${PROJECT_SOURCE_DIR}/test_data" CACHE PATH
  "Directory for downloaded test data and cached archives")
if(NOT IS_ABSOLUTE "${YQUANT_TEST_DATA_DIR}")
  message(FATAL_ERROR "YQUANT_TEST_DATA_DIR must be an absolute path")
endif()

set(yquant_test_data_command
  "${CMAKE_COMMAND}"
  "-DYQUANT_TEST_DATA_DIR=${YQUANT_TEST_DATA_DIR}"
  -P "${CMAKE_CURRENT_LIST_DIR}/DownloadTestData.cmake")
add_custom_target(download_test_data
  COMMAND ${yquant_test_data_command}
  COMMENT "Prepare external test data"
  USES_TERMINAL
  VERBATIM)

if(BUILD_TESTING)
  add_test(NAME TestData.Prepare COMMAND ${yquant_test_data_command})
  set_tests_properties(TestData.Prepare PROPERTIES
    FIXTURES_SETUP yquant_test_data
    LABELS test_data
    TIMEOUT 1300)

  set(yquant_test_properties
    TIMEOUT 60
    FIXTURES_REQUIRED yquant_test_data
    ENVIRONMENT "YQUANT_TEST_DATA_DIR=${YQUANT_TEST_DATA_DIR}")

  add_test(NAME TestData.Download
    COMMAND "${CMAKE_COMMAND}"
      "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DTEST_BINARY_DIR=${PROJECT_BINARY_DIR}/test_data_checks"
      -P "${PROJECT_SOURCE_DIR}/tests/cmake/test_data_test.cmake")
  set_tests_properties(TestData.Download PROPERTIES TIMEOUT 60)
endif()
