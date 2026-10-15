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

add_library(yquant_base STATIC
  src/base/spsc_queue.cc
  src/base/logging.cc
  src/base/logger.cc)
add_library(yquant::base ALIAS yquant_base)
target_include_directories(yquant_base PUBLIC
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(yquant_base PUBLIC
  yquant_compile_options fmt::fmt Threads::Threads)
target_compile_options(yquant_base PRIVATE -Wall -Wextra -Wpedantic)

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
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES TIMEOUT 60)

  # Isolate replacement global new/delete from the other test executable.
  add_executable(spsc_queue_allocation_test
    tests/base/spsc_queue_allocation_test.cc)
  target_link_libraries(spsc_queue_allocation_test PRIVATE
    yquant::base GTest::gtest_main Threads::Threads)
  target_compile_options(spsc_queue_allocation_test PRIVATE
    -Wall -Wextra -Wpedantic)
  gtest_discover_tests(spsc_queue_allocation_test
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES TIMEOUT 60)

  add_executable(logging_test tests/base/logging_test.cc)
  target_include_directories(logging_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(logging_test PRIVATE yquant::base GTest::gtest_main)
  target_compile_options(logging_test PRIVATE -Wall -Wextra -Wpedantic)
  gtest_discover_tests(logging_test
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES TIMEOUT 60)

  add_executable(logging_allocation_test tests/base/logging_allocation_test.cc)
  target_link_libraries(logging_allocation_test PRIVATE
    yquant::base GTest::gtest_main)
  target_compile_options(logging_allocation_test PRIVATE
    -Wall -Wextra -Wpedantic)
  gtest_discover_tests(logging_allocation_test
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES TIMEOUT 60)

  include(cmake/LoggingCompileTests.cmake)
endif()
