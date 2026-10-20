include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# fmt 12.1.0. Its public templates and compiled library share exception settings.
FetchContent_Declare(fmt
  URL https://codeload.github.com/fmtlib/fmt/tar.gz/407c905e45ad75fc29bf0f9bb7c5c2fd3475976f
  URL_HASH SHA256=2bc1fe4a5b6d5d6a614239b4ca1d520e66e152a02d3d262684d26dfd6ab3438a)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_OS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fmt)
target_link_libraries(fmt PUBLIC yquant_compile_options)
target_compile_definitions(fmt PUBLIC FMT_USE_EXCEPTIONS=0)

# simdjson 5.0.4, used only during strategy configuration initialization.
FetchContent_Declare(simdjson
  URL https://codeload.github.com/simdjson/simdjson/tar.gz/refs/tags/v5.0.4
  URL_HASH SHA256=827417bcf88309df170962e7bd1b4b4a4992d5d4e5cb82a0442ada9530e916d2)
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
set(SIMDJSON_INSTALL OFF CACHE BOOL "" FORCE)
set(SIMDJSON_EXCEPTIONS OFF CACHE BOOL "" FORCE)
set(SIMDJSON_ENABLE_THREADS OFF CACHE BOOL "" FORCE)
set(SIMDJSON_STATIC_REFLECTION_MODE OFF CACHE STRING "" FORCE)
function(yquant_make_simdjson_available)
  # Each plugin links its own implementation, without a shared runtime ABI.
  set(BUILD_SHARED_LIBS OFF)
  FetchContent_MakeAvailable(simdjson)
endfunction()
yquant_make_simdjson_available()
target_link_libraries(simdjson PUBLIC yquant_compile_options)
set_target_properties(simdjson PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)

if(BUILD_TESTING)
  # GoogleTest v1.16.0. Pin the complete commit for reproducible builds.
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 6910c9d9165801d8827d628cb72eb7ea9dd538c5)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  foreach(target gtest gtest_main)
    target_link_libraries(${target} PUBLIC yquant_compile_options)
    target_compile_definitions(${target} PUBLIC GTEST_HAS_EXCEPTIONS=0)
  endforeach()
endif()
