include(FetchContent)

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
