add_executable(trading_test
  tests/trading/order_core_test.cc
  tests/trading/order_request_executor_test.cc
  tests/trading/ctp_trading_adapter_test.cc
  tests/trading/futures_exchange_test.cc
  tests/trading/futures_ledger_test.cc
  tests/trading/protocol_test.cc
  tests/trading/connection_test.cc
  tests/trading/trading_test.cc
  tests/trading/trading_config_test.cc
  tests/trading/trading_client_config_test.cc
  tests/trading/mock_trading_adapter_test.cc)
target_include_directories(trading_test PRIVATE "${PROJECT_SOURCE_DIR}/tests" "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(trading_test PRIVATE
  yquant::trading GTest::gtest_main Threads::Threads)
target_compile_options(trading_test PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(trading_test PRIVATE
  YQUANT_TEST_INSTRUMENT_CATALOG_PATH="${PROJECT_SOURCE_DIR}/tests/fixtures/instruments.json")
gtest_discover_tests(trading_test
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 30
  PROPERTIES ${yquant_test_properties})
