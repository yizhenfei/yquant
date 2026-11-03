cmake_minimum_required(VERSION 3.20)
project(yquant VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(yquant_options INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(yquant_options INTERFACE
    -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
endif()

add_library(yquant_model INTERFACE)
target_include_directories(yquant_model INTERFACE include)
target_link_libraries(yquant_model INTERFACE yquant_options)

add_library(yquant_container src/container/container.cc)
target_include_directories(yquant_container PUBLIC include)
target_link_libraries(yquant_container PUBLIC yquant_options)

add_library(yquant_market
  src/market/market_client_factory.cc
  src/market/market_service.cc
  src/market/market_service_registry.cc)
target_link_libraries(yquant_market PUBLIC yquant_model yquant_container)

add_library(yquant_strategy
  src/strategy/market_maker.cc src/strategy/strategy_service.cc)
target_link_libraries(yquant_strategy PUBLIC yquant_market yquant_trading)

add_library(yquant_trading
  src/trading/account_startup.cc
  src/trading/funds_accounting.cc
  src/trading/funds_reservation.cc
  src/trading/local_trading_client.cc
  src/trading/order_id_generator.cc
  src/trading/order_state.cc
  src/trading/simulated_trading_adapter.cc
  src/trading/trade.cc
  src/trading/trading_trade.cc
  src/trading/trading_funds.cc
  src/trading/position_holds.cc
  src/trading/trading_positions.cc
  src/trading/trading_cancel.cc
  src/trading/trading_client_factory.cc
  src/trading/trading_service_registry.cc
  src/trading/trading_service.cc)
target_link_libraries(yquant_trading PUBLIC yquant_model yquant_container)

add_executable(monod
  src/monod/main.cc src/monod/trading_demo.cc src/monod/funds_demo.cc
  src/monod/positions_demo.cc src/monod/reservation_demo.cc
  src/monod/startup_demo.cc src/monod/funds_lifecycle_demo.cc
  src/monod/position_holds_demo.cc src/monod/maker_demo.cc)
target_include_directories(monod PRIVATE examples)
target_link_libraries(monod PRIVATE yquant_strategy yquant_trading)

include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 6910c9d9165801d8827d628cb72eb7ea9dd538c5)
  FetchContent_MakeAvailable(googletest)

  add_executable(market_maker_test tests/market_maker_test.cc)
  target_link_libraries(market_maker_test PRIVATE yquant_strategy GTest::gtest_main)
  add_test(NAME v1_03_maker COMMAND market_maker_test)
  add_test(NAME v1_03_maker_monod
    COMMAND ${CMAKE_COMMAND} -DMONOD=$<TARGET_FILE:monod>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_maker_test.cmake)
  set_tests_properties(v1_03_maker v1_03_maker_monod
    PROPERTIES TIMEOUT 10 LABELS v1_03_maker)

  add_executable(trading_adapter_test tests/trading_adapter_test.cc)
  target_link_libraries(trading_adapter_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_adapter COMMAND trading_adapter_test)
  set_tests_properties(v1_02_adapter
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)
  add_executable(trading_service_test tests/trading_service_test.cc)
  target_link_libraries(trading_service_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_service COMMAND trading_service_test)
  set_tests_properties(v1_02_service
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)
  add_executable(trading_client_test tests/trading_client_test.cc)
  target_link_libraries(trading_client_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_submit COMMAND trading_client_test)
  add_test(NAME v1_02_submit_monod
    COMMAND ${CMAKE_COMMAND} -DMONOD=$<TARGET_FILE:monod>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_order_output_test.cmake)
  set_tests_properties(v1_02_submit v1_02_submit_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)
  add_executable(trading_cancel_test tests/trading_cancel_test.cc)
  target_link_libraries(trading_cancel_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_cancel COMMAND trading_cancel_test)
  add_test(NAME v1_02_cancel_monod
    COMMAND ${CMAKE_COMMAND} -DMONOD=$<TARGET_FILE:monod>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_cancel_output_test.cmake)
  set_tests_properties(v1_02_cancel v1_02_cancel_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(trading_query_test
    tests/trading_query_test.cc tests/trading_cumulative_test.cc)
  target_link_libraries(trading_query_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_fill_query COMMAND trading_query_test)
  set_tests_properties(v1_02_fill_query
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(trading_trade_test tests/trading_trade_test.cc)
  target_link_libraries(trading_trade_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_trade_details COMMAND trading_trade_test)
  add_test(NAME v1_02_trade_details_monod
    COMMAND ${CMAKE_COMMAND} -DMONOD=$<TARGET_FILE:monod>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_fill_output_test.cmake)
  set_tests_properties(v1_02_trade_details v1_02_trade_details_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(trading_position_holds_test tests/trading_position_holds_test.cc)
  target_link_libraries(trading_position_holds_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_positions_lifecycle COMMAND trading_position_holds_test)
  add_test(NAME v1_02_positions_lifecycle_monod
    COMMAND ${CMAKE_COMMAND} -DMONOD=$<TARGET_FILE:monod>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_position_holds_output_test.cmake)
  set_tests_properties(v1_02_positions_lifecycle v1_02_positions_lifecycle_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(trading_funds_lifecycle_test tests/trading_funds_lifecycle_test.cc)
  target_link_libraries(trading_funds_lifecycle_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_funds_lifecycle COMMAND trading_funds_lifecycle_test)
  add_test(NAME v1_02_funds_lifecycle_monod
    COMMAND ${CMAKE_COMMAND} -DMONOD=$<TARGET_FILE:monod>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_funds_lifecycle_output_test.cmake)
  set_tests_properties(v1_02_funds_lifecycle v1_02_funds_lifecycle_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(trading_startup_test tests/trading_startup_test.cc)
  target_link_libraries(trading_startup_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_account_startup COMMAND trading_startup_test)
  add_test(NAME v1_02_account_startup_monod
    COMMAND ${CMAKE_COMMAND} -DMONOD=$<TARGET_FILE:monod>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_startup_output_test.cmake)
  set_tests_properties(v1_02_account_startup v1_02_account_startup_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(currency_test tests/currency_test.cc)
  target_link_libraries(currency_test PRIVATE yquant_model GTest::gtest_main)
  add_test(NAME v1_02_currency COMMAND currency_test)
  set_tests_properties(v1_02_currency
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(trading_funds_test tests/trading_funds_test.cc)
  target_link_libraries(trading_funds_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_funds COMMAND trading_funds_test)
  add_test(NAME v1_02_funds_monod
    COMMAND ${CMAKE_COMMAND} -DMONOD=$<TARGET_FILE:monod>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_funds_output_test.cmake)
  set_tests_properties(v1_02_funds v1_02_funds_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(trading_positions_test tests/trading_positions_test.cc)
  target_link_libraries(trading_positions_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_positions COMMAND trading_positions_test)
  add_test(NAME v1_02_positions_monod
    COMMAND ${CMAKE_COMMAND} -DMONOD=$<TARGET_FILE:monod>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_positions_output_test.cmake)
  set_tests_properties(v1_02_positions v1_02_positions_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(trading_reservation_test tests/trading_reservation_test.cc)
  target_link_libraries(trading_reservation_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_funds_reservation COMMAND trading_reservation_test)
  add_test(NAME v1_02_funds_reservation_monod
    COMMAND ${CMAKE_COMMAND} -DMONOD=$<TARGET_FILE:monod>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_reservation_output_test.cmake)
  set_tests_properties(v1_02_funds_reservation v1_02_funds_reservation_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(single_quote_test tests/single_quote_test.cc)
  target_include_directories(single_quote_test PRIVATE examples)
  target_link_libraries(single_quote_test PRIVATE
    yquant_strategy GTest::gtest_main)
  add_test(NAME v1_01_single_quote COMMAND single_quote_test)
  add_executable(quote_sequence_test tests/quote_sequence_test.cc)
  target_include_directories(quote_sequence_test PRIVATE examples)
  target_link_libraries(quote_sequence_test PRIVATE
    yquant_strategy GTest::gtest_main)
  add_test(NAME v1_01_quote_sequence COMMAND quote_sequence_test)
  add_executable(container_test tests/container_test.cc)
  target_include_directories(container_test PRIVATE examples)
  target_link_libraries(container_test PRIVATE
    yquant_strategy GTest::gtest_main)
  add_test(NAME v1_01_container COMMAND container_test)
  add_executable(market_subscription_test tests/market_subscription_test.cc)
  target_link_libraries(market_subscription_test PRIVATE
    yquant_market GTest::gtest_main)
  add_test(NAME v1_01_subscription COMMAND market_subscription_test)
  add_executable(market_source_test tests/market_source_test.cc)
  target_link_libraries(market_source_test PRIVATE
    yquant_market GTest::gtest_main)
  add_test(NAME v1_01_market_source COMMAND market_source_test)
  add_executable(market_client_factory_test tests/market_client_factory_test.cc)
  target_link_libraries(market_client_factory_test PRIVATE
    yquant_market GTest::gtest_main)
  add_test(NAME v1_01_client_factory COMMAND market_client_factory_test)
  add_test(NAME v1_01_client_factory_monod
    COMMAND ${CMAKE_COMMAND} -DMONOD=$<TARGET_FILE:monod>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_output_test.cmake)
  set_tests_properties(v1_01_single_quote v1_01_quote_sequence
    v1_01_client_factory_monod v1_01_container v1_01_subscription
    v1_01_market_source v1_01_client_factory
    PROPERTIES TIMEOUT 5 LABELS v1_01_market)
endif()
