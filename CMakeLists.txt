cmake_minimum_required(VERSION 3.21)

project(yquant VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  message(FATAL_ERROR "YQuant requires GCC, Clang, or AppleClang")
endif()
if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
  message(FATAL_ERROR "YQuant supports Linux and macOS")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
  message(FATAL_ERROR "The supported Linux target is x86_64")
endif()

include(cmake/CompilerOptions.cmake)
include(cmake/PlatformChecks.cmake)
include(CTest)
include(cmake/Dependencies.cmake)
find_package(Threads REQUIRED)
include(cmake/TestData.cmake)

add_library(yquant_strategy_api INTERFACE)
add_library(yquant::strategy_api ALIAS yquant_strategy_api)
target_include_directories(yquant_strategy_api INTERFACE
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>")
target_link_libraries(yquant_strategy_api INTERFACE yquant_compile_options)

add_library(yquant_md STATIC
  src/md/instrument_catalog.cc
  src/md/mock_quote_config.cc
  src/md/mock_quote_source.cc
  src/md/event_codec.cc
  src/md/quote_codec.cc
  src/md/shm_broadcast.cc)
add_library(yquant::md ALIAS yquant_md)
target_include_directories(yquant_md PUBLIC
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(yquant_md PUBLIC
  yquant::base yquant_compile_options Threads::Threads)
target_link_libraries(yquant_md PRIVATE simdjson::simdjson)
target_compile_options(yquant_md PRIVATE -Wall -Wextra -Wpedantic)

# Host dictionary/receive configuration and termination handling are shared.
add_library(yquant_host_support STATIC
  src/strategyd/market_data_config.cc
  src/strategyd/termination_signals.cc)
target_include_directories(yquant_host_support PUBLIC
  "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(yquant_host_support PUBLIC yquant::strategy_api
  PRIVATE yquant::base yquant::md simdjson::simdjson Threads::Threads)
target_compile_options(yquant_host_support PRIVATE -Wall -Wextra -Wpedantic)

add_library(yquant_trading_wire STATIC
  src/tradingd/protocol.cc
  src/tradingd/connection.cc)
target_include_directories(yquant_trading_wire PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(yquant_trading_wire PUBLIC
  yquant::strategy_api Threads::Threads)
target_compile_options(yquant_trading_wire PRIVATE -Wall -Wextra -Wpedantic)

add_library(yquant_strategyd_runtime STATIC
  src/strategyd/mock_trading_adapter.cc
  src/strategyd/remote_trading.cc
  src/strategyd/shm_market_data_source.cc
  src/strategyd/strategy_config_loader.cc
  src/strategyd/market_data.cc
  src/strategyd/strategy_library.cc
  src/strategyd/strategy_runner.cc
  src/strategyd/trading.cc
  src/strategyd/trading_config.cc)
target_include_directories(yquant_strategyd_runtime PUBLIC
  "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(yquant_strategyd_runtime PUBLIC
  yquant::strategy_api yquant_host_support yquant_trading_wire
  PRIVATE yquant::md simdjson::simdjson Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(yquant_strategyd_runtime PRIVATE
  -Wall -Wextra -Wpedantic)

add_executable(yquant_strategyd src/strategyd/main.cc)
target_link_libraries(yquant_strategyd PRIVATE
  yquant_strategyd_runtime Threads::Threads)
target_compile_options(yquant_strategyd PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(yquant_strategyd PROPERTIES
  OUTPUT_NAME yquant-strategyd
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_library(yquant_tradingd_runtime STATIC
  src/tradingd/config.cc
  src/tradingd/server.cc)
target_include_directories(yquant_tradingd_runtime PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(yquant_tradingd_runtime PUBLIC
  yquant_strategyd_runtime yquant_trading_wire
  PRIVATE simdjson::simdjson Threads::Threads)
target_compile_options(yquant_tradingd_runtime PRIVATE -Wall -Wextra -Wpedantic)
add_executable(yquant_tradingd src/tradingd/main.cc)
target_link_libraries(yquant_tradingd PRIVATE yquant_tradingd_runtime)
target_compile_options(yquant_tradingd PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(yquant_tradingd PROPERTIES
  OUTPUT_NAME yquant-tradingd
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_library(yquant_marketd_runtime STATIC
  src/marketd/adapters.cc
  src/marketd/config.cc
  src/marketd/csv_replay.cc
  src/marketd/recording.cc
  src/marketd/replay_source.cc
  src/marketd/runner.cc
  src/marketd/tcp_transport.cc)
target_include_directories(yquant_marketd_runtime PUBLIC
  "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(yquant_marketd_runtime PUBLIC
  yquant::md yquant::base yquant_host_support
  PRIVATE simdjson::simdjson Threads::Threads)
target_compile_options(yquant_marketd_runtime PRIVATE -Wall -Wextra -Wpedantic)

add_executable(yquant_marketd src/marketd/main.cc)
target_link_libraries(yquant_marketd PRIVATE yquant_marketd_runtime)
target_compile_options(yquant_marketd PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(yquant_marketd PROPERTIES
  OUTPUT_NAME yquant-marketd
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

function(yquant_add_strategy_example name)
  add_library(${name} MODULE examples/strategies/${name}.cc)
  target_link_libraries(${name} PRIVATE yquant::strategy_api)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  set_target_properties(${name} PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/strategies"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
  configure_file(examples/strategies/${name}.json
    "${CMAKE_BINARY_DIR}/strategies/${name}.json" COPYONLY)
endfunction()
yquant_add_strategy_example(lifecycle_strategy)
yquant_add_strategy_example(parameterized_strategy)
yquant_add_strategy_example(quote_observer_strategy)
yquant_add_strategy_example(order_lifecycle_strategy)
yquant_add_strategy_example(shared_account_strategy)
target_link_libraries(lifecycle_strategy PRIVATE simdjson::simdjson)
target_link_libraries(parameterized_strategy PRIVATE simdjson::simdjson)
target_link_libraries(quote_observer_strategy PRIVATE simdjson::simdjson)
target_link_libraries(order_lifecycle_strategy PRIVATE simdjson::simdjson)
target_link_libraries(shared_account_strategy PRIVATE simdjson::simdjson)
configure_file(examples/instruments.json
  "${CMAKE_BINARY_DIR}/instruments.json" COPYONLY)
configure_file(examples/strategies/market_data.json
  "${CMAKE_BINARY_DIR}/strategies/market_data.json" COPYONLY)
configure_file(examples/strategies/shm_market_data.json
  "${CMAKE_BINARY_DIR}/strategies/shm_market_data.json" COPYONLY)
configure_file(examples/strategies/mock_trading.json
  "${CMAKE_BINARY_DIR}/strategies/mock_trading.json" COPYONLY)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/marketd")
foreach(config IN ITEMS mock_source local tcp_source tcp_relay record_local
                        replay_recording)
  configure_file("examples/marketd/${config}.json"
    "${CMAKE_BINARY_DIR}/marketd/${config}.json" COPYONLY)
endforeach()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tradingd")
foreach(config IN ITEMS local client_1 client_2)
  configure_file("examples/tradingd/${config}.json"
    "${CMAKE_BINARY_DIR}/tradingd/${config}.json" COPYONLY)
endforeach()
configure_file(examples/marketd/replay_csv.json.in
  "${CMAKE_BINARY_DIR}/marketd/replay_csv.json" @ONLY)
foreach(config IN ITEMS replay_market_data replay_observer)
  configure_file("examples/strategies/${config}.json"
    "${CMAKE_BINARY_DIR}/strategies/${config}.json" COPYONLY)
endforeach()

add_library(yquant_base STATIC
  src/base/json_config_document.cc
  src/base/path.cc
  src/base/broadcast_queue.cc
  src/base/shared_memory_mapping.cc
  src/base/shm_stream_registry.cc
  src/base/shm_broadcast.cc
  src/base/spsc_queue.cc
  src/base/logging.cc
  src/base/logger.cc)
add_library(yquant::base ALIAS yquant_base)
target_include_directories(yquant_base PUBLIC
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(yquant_base PUBLIC
  yquant_compile_options fmt::fmt Threads::Threads
  PRIVATE simdjson::simdjson)
target_compile_options(yquant_base PRIVATE -Wall -Wextra -Wpedantic)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_library(YQUANT_RT_LIBRARY rt)
  if(YQUANT_RT_LIBRARY)
    target_link_libraries(yquant_base PRIVATE ${YQUANT_RT_LIBRARY})
  endif()
endif()

set(YQUANT_LOG_COMPILED_LEVEL "" CACHE STRING
  "Log compile threshold: empty (Debug DEBUG, otherwise INFO), TRACE, DEBUG, INFO, WARNING, ERROR, or CRITICAL")
set_property(CACHE YQUANT_LOG_COMPILED_LEVEL PROPERTY STRINGS
  "" TRACE DEBUG INFO WARNING ERROR CRITICAL)
if(YQUANT_LOG_COMPILED_LEVEL STREQUAL "")
  target_compile_definitions(yquant_base PUBLIC
    YQ_LOG_COMPILED_LEVEL=$<IF:$<CONFIG:Debug>,YQ_LOG_LEVEL_DEBUG,YQ_LOG_LEVEL_INFO>)
elseif(YQUANT_LOG_COMPILED_LEVEL MATCHES
       "^(TRACE|DEBUG|INFO|WARNING|ERROR|CRITICAL)$")
  target_compile_definitions(yquant_base PUBLIC
    YQ_LOG_COMPILED_LEVEL=YQ_LOG_LEVEL_${YQUANT_LOG_COMPILED_LEVEL})
else()
  message(FATAL_ERROR
    "Unknown YQUANT_LOG_COMPILED_LEVEL: ${YQUANT_LOG_COMPILED_LEVEL}")
endif()

if(BUILD_TESTING)
  include(GoogleTest)

  add_executable(json_config_test
    tests/base/config_error_test.cc
    tests/base/json_config_document_test.cc)
  target_include_directories(json_config_test PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests")
  target_link_libraries(json_config_test PRIVATE
    yquant::base GTest::gtest_main simdjson::simdjson)
  target_compile_options(json_config_test PRIVATE -Wall -Wextra -Wpedantic)
  gtest_discover_tests(json_config_test
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES ${yquant_test_properties})

  add_executable(path_test tests/base/path_test.cc)
  target_include_directories(path_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
  target_link_libraries(path_test PRIVATE yquant::base GTest::gtest_main)
  target_compile_options(path_test PRIVATE -Wall -Wextra -Wpedantic)
  gtest_discover_tests(path_test
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES ${yquant_test_properties})

  add_executable(broadcast_queue_test
    tests/base/broadcast_queue_test.cc
    tests/base/shm_broadcast_test.cc)
  target_include_directories(broadcast_queue_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(broadcast_queue_test PRIVATE
    yquant::base GTest::gtest_main Threads::Threads)
  target_compile_options(broadcast_queue_test PRIVATE -Wall -Wextra -Wpedantic)
  gtest_discover_tests(broadcast_queue_test
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES ${yquant_test_properties})

  add_executable(broadcast_queue_allocation_test
    tests/base/broadcast_queue_allocation_test.cc)
  target_link_libraries(broadcast_queue_allocation_test PRIVATE
    yquant::base GTest::gtest_main)
  target_compile_options(broadcast_queue_allocation_test PRIVATE
    -Wall -Wextra -Wpedantic)
  gtest_discover_tests(broadcast_queue_allocation_test
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES ${yquant_test_properties})

  add_executable(spsc_queue_test
    tests/base/spsc_queue_test.cc
    tests/base/spsc_queue_protocol_test.cc
    tests/base/spsc_queue_layout_test.cc
    tests/base/spsc_queue_concurrency_test.cc
    tests/base/spsc_queue_exhaustive_test.cc)
  target_include_directories(spsc_queue_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(spsc_queue_test PRIVATE
    yquant::base GTest::gtest_main Threads::Threads)
  target_compile_options(spsc_queue_test PRIVATE -Wall -Wextra -Wpedantic)
  gtest_discover_tests(spsc_queue_test
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES ${yquant_test_properties})

  # Isolate replacement global new/delete from the other test executable.
  add_executable(spsc_queue_allocation_test
    tests/base/spsc_queue_allocation_test.cc)
  target_link_libraries(spsc_queue_allocation_test PRIVATE
    yquant::base GTest::gtest_main Threads::Threads)
  target_compile_options(spsc_queue_allocation_test PRIVATE
    -Wall -Wextra -Wpedantic)
  gtest_discover_tests(spsc_queue_allocation_test
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES ${yquant_test_properties})

  add_executable(logging_test tests/base/logging_test.cc)
  target_include_directories(logging_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(logging_test PRIVATE yquant::base GTest::gtest_main)
  target_compile_options(logging_test PRIVATE -Wall -Wextra -Wpedantic)
  gtest_discover_tests(logging_test
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES ${yquant_test_properties})

  add_executable(logging_allocation_test tests/base/logging_allocation_test.cc)
  target_link_libraries(logging_allocation_test PRIVATE
    yquant::base GTest::gtest_main)
  target_compile_options(logging_allocation_test PRIVATE
    -Wall -Wextra -Wpedantic)
  gtest_discover_tests(logging_allocation_test
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES ${yquant_test_properties})

  include(cmake/LoggingCompileTests.cmake)
  include(cmake/StrategydTests.cmake)
  include(cmake/MarketdTests.cmake)
  include(cmake/TradingdTests.cmake)
  foreach(target IN ITEMS yquant_strategyd_test yquant_marketd_test
                          yquant_tradingd_test)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
  endforeach()

  # Keep replacement global allocation functions isolated from other tests.
  add_executable(market_data_allocation_test
    tests/strategyd/market_data_allocation_test.cc)
  target_link_libraries(market_data_allocation_test PRIVATE
    yquant_strategyd_runtime GTest::gtest_main yquant_compile_options)
  target_compile_options(market_data_allocation_test PRIVATE
    -Wall -Wextra -Wpedantic)
  gtest_discover_tests(market_data_allocation_test
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES ${yquant_test_properties})

  add_executable(trading_allocation_test
    tests/strategyd/trading_allocation_test.cc)
  target_link_libraries(trading_allocation_test PRIVATE
    yquant_strategyd_runtime GTest::gtest_main yquant_compile_options)
  target_compile_options(trading_allocation_test PRIVATE
    -Wall -Wextra -Wpedantic)
  gtest_discover_tests(trading_allocation_test
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES ${yquant_test_properties})
endif()
