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
