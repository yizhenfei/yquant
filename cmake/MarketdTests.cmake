add_executable(yquant_md_test
  tests/md/instrument_scope_test.cc
  tests/md/market_data_config_test.cc
  tests/md/instrument_catalog_test.cc
  tests/md/mock_quote_source_test.cc
  tests/md/replay_config_test.cc
  tests/md/mock_quote_config_test.cc
  tests/md/event_filter_test.cc
  tests/md/shm_broadcast_test.cc
  tests/md/quote_codec_test.cc)
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

add_executable(yquant_marketd_test
  tests/marketd/config_test.cc
  tests/marketd/adapters_test.cc
  tests/marketd/runner_test.cc
  tests/marketd/csv_replay_test.cc
  tests/marketd/recording_test.cc
  tests/marketd/replay_source_test.cc
  tests/marketd/tcp_transport_test.cc
  tests/marketd/marketd_integration_test.cc)
target_link_libraries(yquant_marketd_test PRIVATE
  yquant_marketd_objects yquant::trading GTest::gtest_main Threads::Threads)
target_compile_options(yquant_marketd_test PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(yquant_marketd_test PRIVATE
  YQUANT_TEST_INSTRUMENT_CATALOG_PATH="${PROJECT_SOURCE_DIR}/tests/fixtures/instruments.json"
  YQUANT_TEST_EXAMPLE_INSTRUMENT_CATALOG_PATH="${PROJECT_SOURCE_DIR}/examples/instruments.json"
  YQUANT_TEST_MARKETD_PATH="$<TARGET_FILE:yquant_marketd>"
  YQUANT_TEST_TRADINGD_PATH="$<TARGET_FILE:yquant_tradingd>"
  YQUANT_TEST_STRATEGYD_PATH="$<TARGET_FILE:yquant_strategyd>"
  YQUANT_TEST_QUOTE_EXAMPLE_PATH="$<TARGET_FILE:quote_observer_strategy>"
  YQUANT_TEST_QUOTE_EXAMPLE_CONFIG_PATH="${CMAKE_BINARY_DIR}/strategies/quote_observer_strategy.json"
  YQUANT_TEST_ORDER_EXAMPLE_PATH="$<TARGET_FILE:order_lifecycle_strategy>"
  YQUANT_TEST_ORDER_EXAMPLE_CONFIG_PATH="${CMAKE_BINARY_DIR}/strategies/order_lifecycle_strategy.json"
  YQUANT_TEST_TRADING_CONFIG_PATH="${CMAKE_BINARY_DIR}/tradingd/mock_trading.json")
add_dependencies(yquant_marketd_test
  yquant_marketd yquant_strategyd yquant_tradingd quote_observer_strategy order_lifecycle_strategy)
gtest_discover_tests(yquant_marketd_test
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 30
  PROPERTIES ${yquant_test_properties})

# Isolate replacement global new/delete from all other test executables.
if(NOT YQUANT_SANITIZER STREQUAL "thread")
  add_executable(marketd_allocation_test tests/marketd/allocation_test.cc)
  target_include_directories(marketd_allocation_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(marketd_allocation_test PRIVATE
    yquant_marketd_objects GTest::gtest_main Threads::Threads)
  target_compile_options(marketd_allocation_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(marketd_allocation_test PRIVATE
    YQUANT_TEST_INSTRUMENT_CATALOG_PATH="${PROJECT_SOURCE_DIR}/tests/fixtures/instruments.json")
  gtest_discover_tests(marketd_allocation_test
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES ${yquant_test_properties})
endif()
