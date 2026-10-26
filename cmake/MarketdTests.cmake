add_executable(yquant_marketd_test
  tests/marketd/marketd_integration_test.cc)
target_link_libraries(yquant_marketd_test PRIVATE
  yquant::market yquant::trading GTest::gtest_main Threads::Threads)
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
