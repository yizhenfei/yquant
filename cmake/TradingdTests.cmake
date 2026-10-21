add_executable(yquant_tradingd_test
  tests/tradingd/persistence_test.cc
  tests/tradingd/recovery_test.cc
  tests/tradingd/journal_test.cc
  tests/tradingd/protocol_test.cc
  tests/tradingd/connection_test.cc
  tests/tradingd/server_test.cc
  tests/tradingd/process_test.cc
  tests/strategyd/remote_trading_test.cc)
target_link_libraries(yquant_tradingd_test PRIVATE
  yquant_tradingd_runtime GTest::gtest_main Threads::Threads)
target_compile_options(yquant_tradingd_test PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(yquant_tradingd_test PRIVATE
  YQUANT_TEST_TRADINGD_PATH="$<TARGET_FILE:yquant_tradingd>"
  YQUANT_TEST_STRATEGYD_PATH="$<TARGET_FILE:yquant_strategyd>"
  YQUANT_TEST_MARKETD_PATH="$<TARGET_FILE:yquant_marketd>"
  YQUANT_TEST_SHARED_STRATEGY_PATH="$<TARGET_FILE:shared_account_strategy>")
add_dependencies(yquant_tradingd_test
  yquant_tradingd yquant_strategyd yquant_marketd shared_account_strategy)
gtest_discover_tests(yquant_tradingd_test
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 30
  PROPERTIES ${yquant_test_properties})

add_executable(yquant_tradingd_allocation_test tests/tradingd/allocation_test.cc)
target_link_libraries(yquant_tradingd_allocation_test PRIVATE
  yquant_tradingd_runtime GTest::gtest_main Threads::Threads)
target_compile_options(yquant_tradingd_allocation_test PRIVATE
  -Wall -Wextra -Wpedantic)
gtest_discover_tests(yquant_tradingd_allocation_test
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 30
  PROPERTIES ${yquant_test_properties})
