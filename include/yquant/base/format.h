#ifndef YQUANT_BASE_FORMAT_H_
#define YQUANT_BASE_FORMAT_H_

// Type-checked formatting for bounded C strings and stdio streams. Uses
// caller-owned or stack buffers without allocation; failures stay explicit.

#include <stddef.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>

#include "fmt/format.h"

namespace yquant::base {

namespace internal {

// Batches formatted characters without allocating or owning the FILE stream.
class FileFormatBuffer {
 public:
  using value_type = char;
  explicit FileFormatBuffer(std::FILE* stream) noexcept : stream_(stream) {}
  FileFormatBuffer(const FileFormatBuffer&) = delete;
  FileFormatBuffer& operator=(const FileFormatBuffer&) = delete;
  // Supports back_insert_iterator; all writes complete before Finish returns.
  void push_back(char value) noexcept {
    if (used_ == bytes_.size()) {
      Flush();
    }
    bytes_[used_++] = value;
    ++size_;
  }
  int Finish() noexcept {
    Flush();
    return failed_ ||
                   size_ > static_cast<size_t>(std::numeric_limits<int>::max())
               ? -1
               : static_cast<int>(size_);
  }

 private:
  // Records the first short write and suppresses further stream writes.
  void Flush() noexcept {
    if (!failed_ && used_ != 0 &&
        std::fwrite(bytes_.data(), 1, used_, stream_) != used_) {
      failed_ = true;
    }
    used_ = 0;
  }
  std::FILE* stream_;
  std::array<char, 1024> bytes_;
  size_t used_ = 0;
  size_t size_ = 0;
  bool failed_ = false;
};

}  // namespace internal

// Writes type-checked text to a non-null stream without allocating or flushing
// the stream. Returns bytes written, or -1 for an I/O error/size overflow.
// Does not retain the stream or arguments. Serializes the complete message
// with other stdio calls on the stream.
template <typename... Args>
int Print(std::FILE* stream, fmt::format_string<Args...> format,
          Args&&... args) noexcept {
  ::flockfile(stream);
  internal::FileFormatBuffer buffer(stream);
  fmt::format_to(std::back_inserter(buffer), format,
                 std::forward<Args>(args)...);
  const int result = buffer.Finish();
  ::funlockfile(stream);
  return result;
}

// Writes to stdout with the same contract as the stream overload.
template <typename... Args>
int Print(fmt::format_string<Args...> format, Args&&... args) noexcept {
  return Print(stdout, format, std::forward<Args>(args)...);
}

// Writes at most capacity - 1 bytes followed by NUL. Returns the full formatted
// size, or -1 when it cannot fit in int. With zero capacity, output may be
// null. Does not retain output or args. Input storage must not overlap output.
template <typename... Args>
int FormatTo(char* output, size_t capacity, fmt::format_string<Args...> format,
             Args&&... args) noexcept {
  // format_to_n with a zero bound does not dereference the output iterator.
  const auto result = fmt::format_to_n(output, capacity == 0 ? 0 : capacity - 1,
                                       format, std::forward<Args>(args)...);
  if (capacity != 0) {
    output[std::min(capacity - 1, result.size)] = '\0';
  }
  return result.size > static_cast<size_t>(std::numeric_limits<int>::max())
             ? -1
             : static_cast<int>(result.size);
}

// Borrows at most precision bytes up to the first NUL. Negative precision means
// an unbounded C string, matching printf's dynamic string precision semantics.
// Requires readable text through that bound/terminator. Text may be null only
// when precision is zero.
inline std::string_view CStringPrefix(const char* text,
                                      int precision) noexcept {
  if (precision == 0) {
    return {};
  }
  return precision < 0
             ? std::string_view(text)
             : std::string_view(
                   text, ::strnlen(text, static_cast<size_t>(precision)));
}

}  // namespace yquant::base

#endif  // YQUANT_BASE_FORMAT_H_
