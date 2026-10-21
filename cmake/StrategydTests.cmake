function(yquant_add_strategy_fixture name mode)
  add_library(strategyd_fixture_${name} MODULE
    tests/strategyd/strategy_fixture.cc)
  target_link_libraries(strategyd_fixture_${name} PRIVATE
    yquant::strategy_api yquant_compile_options simdjson::simdjson)
  target_compile_definitions(strategyd_fixture_${name} PRIVATE
    YQUANT_TEST_PLUGIN_MODE=${mode})
  target_compile_options(strategyd_fixture_${name} PRIVATE
    -Wall -Wextra -Wpedantic)
  set_target_properties(strategyd_fixture_${name} PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
endfunction()

yquant_add_strategy_fixture(good 0)
yquant_add_strategy_fixture(no_entry 1)
yquant_add_strategy_fixture(bad_abi 2)
yquant_add_strategy_fixture(bad_size 3)
yquant_add_strategy_fixture(no_create 4)
yquant_add_strategy_fixture(no_destroy 5)
yquant_add_strategy_fixture(create_fail 6)
yquant_add_strategy_fixture(create_null 7)
yquant_add_strategy_fixture(create_inconsistent 8)
yquant_add_strategy_fixture(start_fail 9)
yquant_add_strategy_fixture(stop_fail 10)
yquant_add_strategy_fixture(null_plugin 11)
yquant_add_strategy_fixture(missing_dependency 12)
yquant_add_strategy_fixture(typed 13)
yquant_add_strategy_fixture(quotes 14)
yquant_add_strategy_fixture(tick_fail 15)
yquant_add_strategy_fixture(quote_start_fail 16)

function(yquant_add_trading_fixture name mode)
  add_library(strategyd_trading_${name} MODULE
    tests/strategyd/trading_fixture.cc)
  target_link_libraries(strategyd_trading_${name} PRIVATE
    yquant::strategy_api simdjson::simdjson)
  target_compile_definitions(strategyd_trading_${name} PRIVATE
    YQUANT_TEST_TRADING_MODE=${mode})
  target_compile_options(strategyd_trading_${name} PRIVATE
    -Wall -Wextra -Wpedantic)
  set_target_properties(strategyd_trading_${name} PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
endfunction()
yquant_add_trading_fixture(normal 0)
yquant_add_trading_fixture(cancel 1)
yquant_add_trading_fixture(update_fail 2)
yquant_add_trading_fixture(fill_fail 3)
yquant_add_trading_fixture(bulk 4)
yquant_add_trading_fixture(partial_cancel 6)
yquant_add_trading_fixture(insufficient 8)

function(yquant_add_timer_fixture name mode)
  add_library(strategyd_timer_${name} MODULE tests/strategyd/timer_fixture.cc)
  target_link_libraries(strategyd_timer_${name} PRIVATE yquant::strategy_api)
  target_compile_definitions(strategyd_timer_${name} PRIVATE
    YQUANT_TEST_TIMER_MODE=${mode})
  target_compile_options(strategyd_timer_${name} PRIVATE
    -Wall -Wextra -Wpedantic)
  set_target_properties(strategyd_timer_${name} PROPERTIES
    PREFIX "" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
endfunction()
yquant_add_timer_fixture(basic 0)
yquant_add_timer_fixture(cancel 1)
yquant_add_timer_fixture(replace 2)
yquant_add_timer_fixture(rearm 3)
yquant_add_timer_fixture(trading 4)
yquant_add_timer_fixture(fail 5)
yquant_add_timer_fixture(fair 6)
yquant_add_timer_fixture(past 7)

add_executable(strategyd_signal_launcher
  tests/strategyd/signal_inheritance_launcher.cc)
target_link_libraries(strategyd_signal_launcher PRIVATE yquant_compile_options)
target_compile_options(strategyd_signal_launcher PRIVATE
  -Wall -Wextra -Wpedantic)

add_library(strategyd_fixture_dependency SHARED
  tests/strategyd/missing_dependency.cc)
target_link_libraries(strategyd_fixture_dependency PRIVATE yquant::strategy_api)
target_link_libraries(strategyd_fixture_missing_dependency PRIVATE
  strategyd_fixture_dependency)
if(APPLE)
  set(strategy_dependency_rpath "@loader_path")
else()
  set(strategy_dependency_rpath "$ORIGIN")
endif()
set_target_properties(strategyd_fixture_missing_dependency PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "${strategy_dependency_rpath}")

add_executable(yquant_strategyd_test
  tests/strategyd/instrument_catalog_test.cc
  tests/strategyd/replay_market_data_source_test.cc
  tests/strategyd/market_model_test.cc
  tests/strategyd/market_data_config_test.cc
  tests/strategyd/mock_market_data_source_test.cc
  tests/strategyd/shm_market_data_source_test.cc
  tests/strategyd/market_runner_test.cc
  tests/strategyd/strategy_context_test.cc
  tests/strategyd/remote_timer_runner_test.cc
  tests/strategyd/futures_host_test.cc
  tests/strategyd/futures_exchange_test.cc
  tests/strategyd/futures_bbo_test.cc
  tests/strategyd/futures_process_test.cc
  tests/strategyd/futures_ledger_test.cc
  tests/strategyd/trading_test.cc
  tests/strategyd/trading_config_test.cc
  tests/strategyd/mock_trading_adapter_test.cc
  tests/strategyd/trading_runner_test.cc
  tests/strategyd/trading_process_test.cc
  tests/strategyd/timer_test.cc
  tests/strategyd/timer_runner_test.cc
  tests/strategyd/bbo_strategy_test.cc
  tests/strategyd/bbo_process_test.cc
  tests/strategyd/strategy_config_test.cc
  tests/strategyd/strategy_config_loader_test.cc
  tests/strategyd/strategy_runner_test.cc
  tests/strategyd/strategyd_process_test.cc)
target_include_directories(yquant_strategyd_test PRIVATE src)
target_link_libraries(yquant_strategyd_test PRIVATE
  yquant_strategyd_runtime GTest::gtest_main yquant_compile_options)
target_compile_options(yquant_strategyd_test PRIVATE -Wall -Wextra -Wpedantic)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test_data")
configure_file("tests/fixtures/instruments.json"
  "${CMAKE_BINARY_DIR}/test_data/instruments.json" COPYONLY)
foreach(config IN ITEMS lifecycle typed quote_observer mock_market_data
    trading_strategy mock_trading)
  configure_file("tests/strategyd/configs/${config}.json"
    "${CMAKE_BINARY_DIR}/test_data/${config}.json" COPYONLY)
endforeach()
target_compile_definitions(yquant_strategyd_test PRIVATE
  YQUANT_TEST_INSTRUMENT_CATALOG_PATH="${PROJECT_SOURCE_DIR}/tests/fixtures/instruments.json"
  YQUANT_TEST_STRATEGYD_PATH="$<TARGET_FILE:yquant_strategyd>"
  YQUANT_TEST_SIGNAL_LAUNCHER_PATH="$<TARGET_FILE:strategyd_signal_launcher>"
  YQUANT_TEST_EXAMPLE_PATH="$<TARGET_FILE:lifecycle_strategy>"
  YQUANT_TEST_EXAMPLE_CONFIG_PATH="${CMAKE_BINARY_DIR}/strategies/lifecycle_strategy.json"
  YQUANT_TEST_PARAMETERIZED_EXAMPLE_PATH="$<TARGET_FILE:parameterized_strategy>"
  YQUANT_TEST_PARAMETERIZED_EXAMPLE_CONFIG_PATH="${CMAKE_BINARY_DIR}/strategies/parameterized_strategy.json"
  YQUANT_TEST_CONFIG_PATH="${CMAKE_BINARY_DIR}/test_data/lifecycle.json"
  YQUANT_TEST_TYPED_CONFIG_PATH="${CMAKE_BINARY_DIR}/test_data/typed.json")
target_compile_definitions(yquant_strategyd_test PRIVATE
  YQUANT_TEST_QUOTE_CONFIG_PATH="${CMAKE_BINARY_DIR}/test_data/quote_observer.json"
  YQUANT_TEST_MARKET_DATA_CONFIG_PATH="${CMAKE_BINARY_DIR}/test_data/mock_market_data.json"
  YQUANT_TEST_QUOTE_EXAMPLE_PATH="$<TARGET_FILE:quote_observer_strategy>"
  YQUANT_TEST_QUOTE_EXAMPLE_CONFIG_PATH="${CMAKE_BINARY_DIR}/strategies/quote_observer_strategy.json")
target_compile_definitions(yquant_strategyd_test PRIVATE
  YQUANT_TEST_ORDER_EXAMPLE_PATH="$<TARGET_FILE:order_lifecycle_strategy>"
  YQUANT_TEST_ORDER_EXAMPLE_CONFIG_PATH="${CMAKE_BINARY_DIR}/strategies/order_lifecycle_strategy.json"
  YQUANT_TEST_TRADING_CONFIG_PATH="${CMAKE_BINARY_DIR}/test_data/mock_trading.json"
  YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH="${CMAKE_BINARY_DIR}/test_data/trading_strategy.json")
target_compile_definitions(yquant_strategyd_test PRIVATE
  YQUANT_TEST_BBO_EXAMPLE_PATH="$<TARGET_FILE:bbo_market_making_strategy>"
  YQUANT_TEST_BBO_EXAMPLE_CONFIG_PATH="${CMAKE_BINARY_DIR}/strategies/bbo_market_making_strategy.json"
  YQUANT_TEST_BBO_TRADING_CONFIG_PATH="${CMAKE_BINARY_DIR}/strategies/bbo_mock_trading.json")
target_compile_definitions(yquant_strategyd_test PRIVATE
  YQUANT_TEST_FUTURES_BBO_PATH="$<TARGET_FILE:futures_bbo_strategy>"
  YQUANT_TEST_FUTURES_STRATEGY_CONFIG_PATH="${CMAKE_BINARY_DIR}/strategies/futures_bbo_strategy.json"
  YQUANT_TEST_FUTURES_MARKET_CONFIG_PATH="${CMAKE_BINARY_DIR}/strategies/futures_market_data.json"
  YQUANT_TEST_FUTURES_TRADING_CONFIG_PATH="${CMAKE_BINARY_DIR}/strategies/futures_trading.json")
add_dependencies(yquant_strategyd_test
  futures_bbo_strategy
  yquant_strategyd lifecycle_strategy parameterized_strategy
  quote_observer_strategy order_lifecycle_strategy bbo_market_making_strategy
  strategyd_signal_launcher)

foreach(name IN ITEMS basic cancel replace rearm trading fail fair past)
  string(TOUPPER "${name}" upper_name)
  target_compile_definitions(yquant_strategyd_test PRIVATE
    YQUANT_TEST_TIMER_${upper_name}_PATH="$<TARGET_FILE:strategyd_timer_${name}>")
  add_dependencies(yquant_strategyd_test strategyd_timer_${name})
endforeach()

foreach(name IN ITEMS normal cancel update_fail fill_fail bulk partial_cancel
    insufficient)
  string(TOUPPER "${name}" upper_name)
  target_compile_definitions(yquant_strategyd_test PRIVATE
    YQUANT_TEST_TRADING_${upper_name}_PATH="$<TARGET_FILE:strategyd_trading_${name}>")
  add_dependencies(yquant_strategyd_test strategyd_trading_${name})
endforeach()

foreach(name IN ITEMS good no_entry bad_abi bad_size no_create no_destroy
    create_fail create_null create_inconsistent start_fail stop_fail null_plugin
    missing_dependency typed quotes tick_fail quote_start_fail)
  string(TOUPPER "${name}" upper_name)
  target_compile_definitions(yquant_strategyd_test PRIVATE
    YQUANT_TEST_PLUGIN_${upper_name}_PATH="$<TARGET_FILE:strategyd_fixture_${name}>")
  add_dependencies(yquant_strategyd_test strategyd_fixture_${name})
endforeach()

gtest_discover_tests(yquant_strategyd_test
  NO_PRETTY_VALUES
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 30
  PROPERTIES ${yquant_test_properties})
