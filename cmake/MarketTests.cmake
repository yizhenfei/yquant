add_executable(yquant_market_test
  tests/market/instrument_scope_test.cc
  tests/market/market_data_config_test.cc
  tests/market/instrument_catalog_test.cc
  tests/market/mock_quote_source_test.cc
  tests/market/replay_config_test.cc
  tests/market/mock_quote_config_test.cc
  tests/market/event_filter_test.cc
  tests/market/shm_broadcast_test.cc
  tests/market/quote_codec_test.cc
  tests/market/pipeline_config_test.cc
  tests/market/sources_test.cc
  tests/market/pipeline_runner_test.cc
  tests/market/csv_replay_test.cc
  tests/market/recording_test.cc
  tests/market/replay_source_test.cc
  tests/market/tcp_transport_test.cc)
target_include_directories(yquant_market_test PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests")
target_compile_definitions(yquant_market_test PRIVATE YQUANT_TEST_INSTRUMENT_CATALOG_PATH="${PROJECT_SOURCE_DIR}/tests/fixtures/instruments.json")
target_link_libraries(yquant_market_test PRIVATE
  yquant::market GTest::gtest_main Threads::Threads)
target_compile_options(yquant_market_test PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(yquant_market_test
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 30
  PROPERTIES ${yquant_test_properties})

# Isolate replacement global new/delete from all other test executables.
if(NOT YQUANT_SANITIZER STREQUAL "thread")
  add_executable(market_pipeline_allocation_test tests/market/pipeline_allocation_test.cc)
  target_include_directories(market_pipeline_allocation_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(market_pipeline_allocation_test PRIVATE
    yquant::market GTest::gtest_main Threads::Threads)
  target_compile_options(market_pipeline_allocation_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(market_pipeline_allocation_test PRIVATE
    YQUANT_TEST_INSTRUMENT_CATALOG_PATH="${PROJECT_SOURCE_DIR}/tests/fixtures/instruments.json")
  gtest_discover_tests(market_pipeline_allocation_test
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES ${yquant_test_properties})
endif()
