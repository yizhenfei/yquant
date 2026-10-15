include(CheckCXXSourceCompiles)
check_cxx_source_compiles([[
  #include <atomic>
  #include <cstddef>
  #include <cstdint>
  #include <span>
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  int main() {
    std::byte storage[4]{};
    const std::span<std::byte> bytes(storage);
    return bytes.size() == 4 ? 0 : 1;
  }
]] YQUANT_HAS_REQUIRED_CXX20_SUPPORT)
if(NOT YQUANT_HAS_REQUIRED_CXX20_SUPPORT)
  message(FATAL_ERROR
    "YQuant requires C++20 std::span and always-lock-free 64-bit atomics")
endif()
