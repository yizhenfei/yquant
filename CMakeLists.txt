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

add_library(yquant_market src/market/market_service.cc)
target_link_libraries(yquant_market PUBLIC yquant_model)

add_library(yquant_strategy src/strategy/strategy_service.cc)
target_link_libraries(yquant_strategy PUBLIC yquant_market)

add_executable(monod src/monod/main.cc)
target_include_directories(monod PRIVATE examples)
target_link_libraries(monod PRIVATE yquant_strategy yquant_container)

include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 6910c9d9165801d8827d628cb72eb7ea9dd538c5)
  FetchContent_MakeAvailable(googletest)

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
    yquant_container yquant_strategy GTest::gtest_main)
  add_test(NAME v1_01_container COMMAND container_test)
  add_executable(market_subscription_test tests/market_subscription_test.cc)
  target_link_libraries(market_subscription_test PRIVATE
    yquant_market GTest::gtest_main)
  add_test(NAME v1_01_subscription COMMAND market_subscription_test)
  add_test(NAME v1_01_subscription_monod
    COMMAND ${CMAKE_COMMAND} -DMONOD=$<TARGET_FILE:monod>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/monod_output_test.cmake)
  set_tests_properties(v1_01_single_quote v1_01_quote_sequence
    v1_01_subscription_monod v1_01_container v1_01_subscription
    PROPERTIES TIMEOUT 5 LABELS v1_01_market)
endif()
