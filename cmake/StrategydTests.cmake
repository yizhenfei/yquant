function(yquant_add_strategy_fixture name mode)
  add_library(strategyd_fixture_${name} MODULE
    tests/strategyd/strategy_fixture.cc)
  target_link_libraries(strategyd_fixture_${name} PRIVATE
    yquant::strategy_api yquant_compile_options)
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
  tests/strategyd/strategy_runtime_test.cc
  tests/strategyd/strategyd_process_test.cc)
target_include_directories(yquant_strategyd_test PRIVATE src)
target_link_libraries(yquant_strategyd_test PRIVATE
  yquant_strategyd_runtime GTest::gtest_main yquant_compile_options)
target_compile_options(yquant_strategyd_test PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(yquant_strategyd_test PRIVATE
  YQUANT_TEST_STRATEGYD_PATH="$<TARGET_FILE:yquant_strategyd>"
  YQUANT_TEST_EXAMPLE_PATH="$<TARGET_FILE:lifecycle_strategy>")
add_dependencies(yquant_strategyd_test yquant_strategyd lifecycle_strategy)

foreach(name IN ITEMS good no_entry bad_abi bad_size no_create no_destroy
    create_fail create_null create_inconsistent start_fail stop_fail null_plugin
    missing_dependency)
  string(TOUPPER "${name}" upper_name)
  target_compile_definitions(yquant_strategyd_test PRIVATE
    YQUANT_TEST_PLUGIN_${upper_name}_PATH="$<TARGET_FILE:strategyd_fixture_${name}>")
  add_dependencies(yquant_strategyd_test strategyd_fixture_${name})
endforeach()

gtest_discover_tests(yquant_strategyd_test
  NO_PRETTY_VALUES
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 30
  PROPERTIES TIMEOUT 30)
