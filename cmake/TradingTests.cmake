add_executable(trading_test
  tests/trading/protocol_test.cc
  tests/trading/connection_test.cc
  tests/trading/trading_test.cc
  tests/trading/trading_config_test.cc
  tests/trading/mock_trading_adapter_test.cc)
target_include_directories(trading_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(trading_test PRIVATE
  yquant::trading GTest::gtest_main Threads::Threads)
target_compile_options(trading_test PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(trading_test
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 30
  PROPERTIES ${yquant_test_properties})
