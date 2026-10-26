add_executable(yquant_md_test
  tests/md/instrument_scope_test.cc
  tests/md/market_data_config_test.cc
  tests/md/instrument_catalog_test.cc
  tests/md/mock_quote_source_test.cc
  tests/md/replay_config_test.cc
  tests/md/mock_quote_config_test.cc
  tests/md/event_filter_test.cc
  tests/md/shm_broadcast_test.cc
  tests/md/quote_codec_test.cc
  tests/md/pipeline_config_test.cc
  tests/md/sources_test.cc
  tests/md/pipeline_runner_test.cc
  tests/md/csv_replay_test.cc
  tests/md/recording_test.cc
  tests/md/replay_source_test.cc
  tests/md/tcp_transport_test.cc)
target_include_directories(yquant_md_test PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests")
target_compile_definitions(yquant_md_test PRIVATE YQUANT_TEST_INSTRUMENT_CATALOG_PATH="${PROJECT_SOURCE_DIR}/tests/fixtures/instruments.json")
target_link_libraries(yquant_md_test PRIVATE
  yquant::md GTest::gtest_main Threads::Threads)
target_compile_options(yquant_md_test PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(yquant_md_test
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 30
  PROPERTIES ${yquant_test_properties})

# Isolate replacement global new/delete from all other test executables.
if(NOT YQUANT_SANITIZER STREQUAL "thread")
  add_executable(md_pipeline_allocation_test tests/md/pipeline_allocation_test.cc)
  target_include_directories(md_pipeline_allocation_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(md_pipeline_allocation_test PRIVATE
    yquant::md GTest::gtest_main Threads::Threads)
  target_compile_options(md_pipeline_allocation_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(md_pipeline_allocation_test PRIVATE
    YQUANT_TEST_INSTRUMENT_CATALOG_PATH="${PROJECT_SOURCE_DIR}/tests/fixtures/instruments.json")
  gtest_discover_tests(md_pipeline_allocation_test
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES ${yquant_test_properties})
endif()
