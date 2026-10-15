set(YQUANT_SANITIZER "" CACHE STRING
  "Sanitizer configuration: empty, address, or thread")
set_property(CACHE YQUANT_SANITIZER PROPERTY STRINGS "" address thread)

add_library(yquant_compile_options INTERFACE)
target_compile_features(yquant_compile_options INTERFACE cxx_std_20)
target_compile_options(yquant_compile_options INTERFACE -fno-exceptions)

if(YQUANT_SANITIZER STREQUAL "address")
  target_compile_options(yquant_compile_options INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(yquant_compile_options INTERFACE
    -fsanitize=address,undefined)
elseif(YQUANT_SANITIZER STREQUAL "thread")
  target_compile_options(yquant_compile_options INTERFACE
    -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(yquant_compile_options INTERFACE -fsanitize=thread)
elseif(NOT YQUANT_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown YQUANT_SANITIZER: ${YQUANT_SANITIZER}")
endif()
