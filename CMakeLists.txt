cmake_minimum_required(VERSION 3.20)
project(yquant VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

# Both supported platforms use the native CTP market library.
set(YQUANT_CTP_SDK_ROOT "${PROJECT_SOURCE_DIR}/vendor/ctp" CACHE PATH
    "Directory containing the platform CTP market and trader SDKs")
if(APPLE)
  set(ctp_framework "${YQUANT_CTP_SDK_ROOT}/thostmduserapi_se.framework")
  set(ctp_headers "${ctp_framework}/Headers")
  set(ctp_library "${ctp_framework}/thostmduserapi_se")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  unset(ctp_headers CACHE)
  unset(ctp_library CACHE)
  find_path(ctp_headers ThostFtdcMdApi.h PATHS "${YQUANT_CTP_SDK_ROOT}"
    PATH_SUFFIXES include/ctp include NO_DEFAULT_PATH)
  find_file(ctp_library NAMES libthostmduserapi_se.so thostmduserapi_se.so
    PATHS "${YQUANT_CTP_SDK_ROOT}" PATH_SUFFIXES lib NO_DEFAULT_PATH)
else()
  message(FATAL_ERROR "The SimNow market SDK requires Linux or macOS")
endif()
if(NOT EXISTS "${ctp_headers}/ThostFtdcMdApi.h" OR NOT EXISTS "${ctp_library}"
   OR NOT EXISTS "${ctp_headers}/ThostFtdcUserApiDataType.h"
   OR NOT EXISTS "${ctp_headers}/ThostFtdcUserApiStruct.h")
  message(FATAL_ERROR
    "CTP market SDK missing. Set YQUANT_CTP_SDK_ROOT to the platform SDK; see README.")
endif()
add_library(yquant_ctp_md SHARED IMPORTED)
set_target_properties(yquant_ctp_md PROPERTIES IMPORTED_LOCATION "${ctp_library}"
  INTERFACE_INCLUDE_DIRECTORIES "${ctp_headers}")
find_package(Threads REQUIRED)
add_library(yquant_simnow_market src/market/simnow_quote_source.cc)
target_link_libraries(yquant_simnow_market PUBLIC yquant_market PRIVATE yquant_ctp_md Threads::Threads)
add_library(yquant_strategy
  src/strategy/market_maker.cc src/strategy/strategy_service.cc)
target_link_libraries(yquant_strategy PUBLIC yquant_market yquant_trading)

add_library(yquant_trading
  src/trading/account_recovery.cc
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

if(APPLE)
  set(td_framework "${YQUANT_CTP_SDK_ROOT}/thosttraderapi_se.framework")
  set(td_headers "${td_framework}/Headers")
  set(td_library "${td_framework}/thosttraderapi_se")
else()
  set(td_headers "${ctp_headers}")
  unset(td_library CACHE)
  find_file(td_library NAMES libthosttraderapi_se.so thosttraderapi_se.so
    PATHS "${YQUANT_CTP_SDK_ROOT}" PATH_SUFFIXES lib NO_DEFAULT_PATH)
endif()
if(NOT EXISTS "${td_headers}/ThostFtdcTraderApi.h" OR NOT EXISTS "${td_library}"
   OR NOT EXISTS "${td_headers}/ThostFtdcUserApiStruct.h"
   OR NOT EXISTS "${td_headers}/ThostFtdcUserApiDataType.h")
  message(FATAL_ERROR "CTP trader SDK missing under YQUANT_CTP_SDK_ROOT")
endif()
add_library(yquant_ctp_td SHARED IMPORTED)
set_target_properties(yquant_ctp_td PROPERTIES IMPORTED_LOCATION "${td_library}"
  INTERFACE_INCLUDE_DIRECTORIES "${td_headers}")
add_library(yquant_simnow_trading src/trading/simnow_trading_adapter.cc
  src/trading/simnow_contract_query.cc src/trading/simnow_order_history.cc
  src/trading/simnow_order_tag.cc src/trading/simnow_position_costs.cc
  src/trading/simnow_order_holds.cc src/trading/simnow_account_recovery.cc
  src/trading/simnow_submissions.cc src/trading/simnow_cancellations.cc)
target_link_libraries(yquant_simnow_trading PUBLIC yquant_trading
  PRIVATE yquant_ctp_td Threads::Threads)
if(APPLE)
  target_compile_definitions(yquant_ctp_td INTERFACE YQUANT_CTP_MAC_LOGIN)
endif()

add_library(yquant_strategy_host src/strategy/plugin.cc src/strategy/loaded_strategy.cc)
target_link_libraries(yquant_strategy_host PUBLIC yquant_market yquant_trading
  PRIVATE ${CMAKE_DL_LIBS})
add_library(yquant_market_maker SHARED src/strategy/market_maker_plugin.cc)
target_link_libraries(yquant_market_maker PRIVATE yquant_strategy)
set_target_properties(yquant_market_maker PROPERTIES
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
# Keep archive definitions private too, including the shared C++ interface RTTI.
if(APPLE)
  target_link_options(yquant_market_maker PRIVATE
    "LINKER:-exported_symbol,_yquant_strategy_plugin")
else()
  target_link_options(yquant_market_maker PRIVATE "LINKER:--exclude-libs,ALL")
endif()

file(GENERATE OUTPUT "$<TARGET_FILE_DIR:yquant_market_maker>/market_maker.conf"
  INPUT "${CMAKE_CURRENT_SOURCE_DIR}/configs/market_maker.conf.in")

add_executable(monod src/monod/main.cc)
target_link_libraries(monod PRIVATE yquant_options)

include(CTest)
if(BUILD_TESTING)
  add_library(verification_market OBJECT tests/scenarios/simnow_market_demo.cc)
  target_include_directories(verification_market PRIVATE examples)
  target_link_libraries(verification_market PRIVATE yquant_simnow_market yquant_strategy)
  add_library(verification_trader OBJECT tests/scenarios/trader_login.cc)
  target_link_libraries(verification_trader PRIVATE yquant_simnow_trading)

  add_executable(simnow_market_probe tests/scenarios/market_main.cc
    $<TARGET_OBJECTS:verification_market>)
  target_link_libraries(simnow_market_probe PRIVATE yquant_simnow_market yquant_strategy)
  add_executable(simnow_trader_probe tests/scenarios/trader_main.cc
    $<TARGET_OBJECTS:verification_trader>)
  target_link_libraries(simnow_trader_probe PRIVATE yquant_simnow_trading)
  add_executable(yquant_scenarios tests/scenarios/main.cc
    tests/scenarios/trading_demo.cc tests/scenarios/funds_demo.cc
    tests/scenarios/positions_demo.cc tests/scenarios/reservation_demo.cc
    tests/scenarios/startup_demo.cc tests/scenarios/funds_lifecycle_demo.cc
    tests/scenarios/position_holds_demo.cc tests/scenarios/maker_demo.cc)
  target_include_directories(yquant_scenarios PRIVATE examples)
  target_link_libraries(yquant_scenarios PRIVATE yquant_strategy)
  add_custom_target(verify_simnow_market
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_simnow_market
      --probe $<TARGET_FILE:simnow_market_probe>
    DEPENDS simnow_market_probe USES_TERMINAL VERBATIM)

  add_test(NAME v1_06_monod_cli COMMAND ${CMAKE_COMMAND}
    -DMONOD=$<TARGET_FILE:monod> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_production_test.cmake)
  add_test(NAME v1_06_production_build COMMAND ${CMAKE_COMMAND}
    -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}
    -DBINARY=${CMAKE_CURRENT_BINARY_DIR}/production-check
    -DGENERATOR=${CMAKE_GENERATOR} -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
    -DCOMPILER=${CMAKE_CXX_COMPILER} -DSDK=${YQUANT_CTP_SDK_ROOT}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/production_build_test.cmake)
  set_tests_properties(v1_06_monod_cli v1_06_production_build
    PROPERTIES TIMEOUT 75 LABELS v1_06_production)

  include(FetchContent)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 6910c9d9165801d8827d628cb72eb7ea9dd538c5)
  FetchContent_MakeAvailable(googletest)

  foreach(fixture IN ITEMS fixed bad_version fail_create)
    add_library(strategy_${fixture} SHARED tests/plugins/strategy_fixture.cc)
    target_link_libraries(strategy_${fixture} PRIVATE yquant_trading yquant_market)
    set_target_properties(strategy_${fixture} PROPERTIES
      CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    if(APPLE)
      target_link_options(strategy_${fixture} PRIVATE
        "LINKER:-exported_symbol,_yquant_strategy_plugin")
    else()
      target_link_options(strategy_${fixture} PRIVATE "LINKER:--exclude-libs,ALL")
    endif()
  endforeach()
  target_compile_definitions(strategy_bad_version PRIVATE TEST_VERSION=99)
  target_compile_definitions(strategy_fail_create PRIVATE FAIL_CREATE)
  add_library(strategy_no_entry MODULE tests/plugins/no_entry.cc)
  add_executable(strategy_plugin_test tests/strategy_plugin_test.cc)
  target_link_libraries(strategy_plugin_test PRIVATE yquant_strategy_host
    GTest::gtest_main ${CMAKE_DL_LIBS})
  target_compile_definitions(strategy_plugin_test PRIVATE
    MAKER_PLUGIN="$<TARGET_FILE:yquant_market_maker>"
    FIXED_PLUGIN="$<TARGET_FILE:strategy_fixed>"
    BAD_VERSION_PLUGIN="$<TARGET_FILE:strategy_bad_version>"
    FAIL_CREATE_PLUGIN="$<TARGET_FILE:strategy_fail_create>"
    NO_ENTRY_PLUGIN="$<TARGET_FILE:strategy_no_entry>")
  add_dependencies(strategy_plugin_test yquant_market_maker strategy_fixed
    strategy_bad_version strategy_fail_create strategy_no_entry)
  add_test(NAME v1_06_strategy_plugin COMMAND strategy_plugin_test)
  set_tests_properties(v1_06_strategy_plugin PROPERTIES TIMEOUT 10 LABELS v1_06_production)

  add_executable(verify_simnow_trading tests/verify_simnow_trading.cc)
  target_link_libraries(verify_simnow_trading PRIVATE yquant_simnow_trading Threads::Threads)
  add_executable(verify_simnow_account_startup tests/verify_simnow_account_startup.cc)
  target_link_libraries(verify_simnow_account_startup PRIVATE yquant_simnow_trading Threads::Threads)

  include(tests/generate_trader_stubs.cmake)
  add_executable(simnow_trader_login_test tests/simnow_trader_login_test.cc
    $<TARGET_OBJECTS:verification_trader>)
  target_include_directories(simnow_trader_login_test PRIVATE tests/scenarios src/trading)
  target_include_directories(simnow_trader_login_test SYSTEM PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/trader_test_sdk")
  target_link_libraries(simnow_trader_login_test PRIVATE yquant_simnow_trading yquant_strategy
    yquant_ctp_td GTest::gtest_main Threads::Threads)
  add_test(NAME v1_05_trader_login COMMAND simnow_trader_login_test
    --gtest_filter=-StartupSnapshot*:ContractTerms*:OrderHistory*:PositionCosts*:OrderHolds*:RecoveredAccounting*:AccountBoundary*:SimNowSubmit*:SimNowCancel*:SimNowFills*)
  add_test(NAME v1_05_startup_snapshot COMMAND simnow_trader_login_test
    --gtest_filter=StartupSnapshot*)
  add_test(NAME v1_05_contract_terms COMMAND simnow_trader_login_test
    --gtest_filter=ContractTerms*)
  add_test(NAME v1_05_order_history COMMAND simnow_trader_login_test
    --gtest_filter=OrderHistory*)
  add_test(NAME v1_05_position_costs COMMAND simnow_trader_login_test
    --gtest_filter=PositionCosts*)
  add_test(NAME v1_05_order_holds COMMAND simnow_trader_login_test
    --gtest_filter=OrderHolds*)
  add_test(NAME v1_05_recovered_accounting COMMAND simnow_trader_login_test
    --gtest_filter=RecoveredAccounting*)
  add_test(NAME v1_05_account_startup COMMAND simnow_trader_login_test
    --gtest_filter=AccountBoundary*)
  add_test(NAME v1_05_submit COMMAND simnow_trader_login_test
    --gtest_filter=SimNowSubmit*)
  add_test(NAME v1_05_cancel COMMAND simnow_trader_login_test
    --gtest_filter=SimNowCancel*)
  add_test(NAME v1_05_fills COMMAND simnow_trader_login_test
    --gtest_filter=SimNowFills*)
  set_tests_properties(v1_05_fills v1_05_cancel v1_05_submit v1_05_account_startup v1_05_recovered_accounting v1_05_order_holds v1_05_trader_login v1_05_order_history v1_05_position_costs v1_05_startup_snapshot
    v1_05_contract_terms PROPERTIES TIMEOUT 10
    LABELS v1_05_simnow_trading ENVIRONMENT
    "YQUANT_CTP_BROKER_ID=test;YQUANT_CTP_USER_ID=test;YQUANT_CTP_PASSWORD=test;YQUANT_CTP_APP_ID=test;YQUANT_CTP_AUTH_CODE=test")

  add_executable(simnow_market_test tests/simnow_market_test.cc)
  target_include_directories(simnow_market_test PRIVATE src/market examples)
  target_link_libraries(simnow_market_test PRIVATE yquant_simnow_market yquant_ctp_md
    yquant_strategy GTest::gtest_main Threads::Threads)
  add_test(NAME v1_04_simnow_market COMMAND simnow_market_test)
  add_executable(simnow_market_fixture tests/simnow_market_fixture.cc
    $<TARGET_OBJECTS:verification_market>)
  target_include_directories(simnow_market_fixture PRIVATE src/market tests/scenarios)
  target_link_libraries(simnow_market_fixture PRIVATE yquant_simnow_market
    yquant_ctp_md yquant_strategy Threads::Threads)
  add_test(NAME v1_04_simnow_market_monod COMMAND
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_simnow_market
    --probe $<TARGET_FILE:simnow_market_fixture> --front tcp://fixture:1
    --symbol IFtest --count 2 --timeout 2)
  add_test(NAME v1_04_simnow_market_cli COMMAND ${CMAKE_COMMAND}
    -DFIXTURE=$<TARGET_FILE:simnow_market_fixture> -DPROBE=$<TARGET_FILE:simnow_market_probe>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/simnow_market_cli_test.cmake)
  set_tests_properties(v1_04_simnow_market_monod v1_04_simnow_market_cli
    PROPERTIES ENVIRONMENT
    "YQUANT_CTP_BROKER_ID=test;YQUANT_CTP_USER_ID=test;YQUANT_CTP_PASSWORD=test")
  set_tests_properties(v1_04_simnow_market v1_04_simnow_market_monod v1_04_simnow_market_cli
    PROPERTIES TIMEOUT 10 LABELS v1_04_simnow_market)

  add_executable(market_maker_test tests/market_maker_test.cc)
  target_link_libraries(market_maker_test PRIVATE yquant_strategy GTest::gtest_main)
  add_test(NAME v1_03_maker COMMAND market_maker_test)
  add_test(NAME v1_03_maker_monod
    COMMAND ${CMAKE_COMMAND} -DSCENARIOS=$<TARGET_FILE:yquant_scenarios>
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
    COMMAND ${CMAKE_COMMAND} -DSCENARIOS=$<TARGET_FILE:yquant_scenarios>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_order_output_test.cmake)
  set_tests_properties(v1_02_submit v1_02_submit_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)
  add_executable(trading_cancel_test tests/trading_cancel_test.cc)
  target_link_libraries(trading_cancel_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_cancel COMMAND trading_cancel_test)
  add_test(NAME v1_02_cancel_monod
    COMMAND ${CMAKE_COMMAND} -DSCENARIOS=$<TARGET_FILE:yquant_scenarios>
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
    COMMAND ${CMAKE_COMMAND} -DSCENARIOS=$<TARGET_FILE:yquant_scenarios>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_fill_output_test.cmake)
  set_tests_properties(v1_02_trade_details v1_02_trade_details_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(trading_position_holds_test tests/trading_position_holds_test.cc)
  target_link_libraries(trading_position_holds_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_positions_lifecycle COMMAND trading_position_holds_test)
  add_test(NAME v1_02_positions_lifecycle_monod
    COMMAND ${CMAKE_COMMAND} -DSCENARIOS=$<TARGET_FILE:yquant_scenarios>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_position_holds_output_test.cmake)
  set_tests_properties(v1_02_positions_lifecycle v1_02_positions_lifecycle_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(trading_funds_lifecycle_test tests/trading_funds_lifecycle_test.cc)
  target_link_libraries(trading_funds_lifecycle_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_funds_lifecycle COMMAND trading_funds_lifecycle_test)
  add_test(NAME v1_02_funds_lifecycle_monod
    COMMAND ${CMAKE_COMMAND} -DSCENARIOS=$<TARGET_FILE:yquant_scenarios>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_funds_lifecycle_output_test.cmake)
  set_tests_properties(v1_02_funds_lifecycle v1_02_funds_lifecycle_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(trading_startup_test tests/trading_startup_test.cc)
  target_link_libraries(trading_startup_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_account_startup COMMAND trading_startup_test)
  add_test(NAME v1_02_account_startup_monod
    COMMAND ${CMAKE_COMMAND} -DSCENARIOS=$<TARGET_FILE:yquant_scenarios>
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
    COMMAND ${CMAKE_COMMAND} -DSCENARIOS=$<TARGET_FILE:yquant_scenarios>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_funds_output_test.cmake)
  set_tests_properties(v1_02_funds v1_02_funds_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(trading_positions_test tests/trading_positions_test.cc)
  target_link_libraries(trading_positions_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_positions COMMAND trading_positions_test)
  add_test(NAME v1_02_positions_monod
    COMMAND ${CMAKE_COMMAND} -DSCENARIOS=$<TARGET_FILE:yquant_scenarios>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_positions_output_test.cmake)
  set_tests_properties(v1_02_positions v1_02_positions_monod
    PROPERTIES TIMEOUT 5 LABELS v1_02_order)

  add_executable(trading_reservation_test tests/trading_reservation_test.cc)
  target_link_libraries(trading_reservation_test PRIVATE
    yquant_trading GTest::gtest_main)
  add_test(NAME v1_02_funds_reservation COMMAND trading_reservation_test)
  add_test(NAME v1_02_funds_reservation_monod
    COMMAND ${CMAKE_COMMAND} -DSCENARIOS=$<TARGET_FILE:yquant_scenarios>
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
    COMMAND ${CMAKE_COMMAND} -DSCENARIOS=$<TARGET_FILE:yquant_scenarios>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_output_test.cmake)
  set_tests_properties(v1_01_single_quote v1_01_quote_sequence
    v1_01_client_factory_monod v1_01_container v1_01_subscription
    v1_01_market_source v1_01_client_factory
    PROPERTIES TIMEOUT 5 LABELS v1_01_market)
endif()
