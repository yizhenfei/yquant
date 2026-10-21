#ifndef YQUANT_TRADINGD_STATE_CODEC_H_
#define YQUANT_TRADINGD_STATE_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace yquant::tradingd {

// Bounded little-endian cursor for gateway control records. Neither cursor
// allocates. Once exhausted/malformed, it remains failed; callers publish only
// a completely successful encoding and validate done() before installing data.
class StateWriter {
 public:
  explicit StateWriter(std::span<std::byte> output) noexcept
      : output_(output) {}
  void U64(std::uint64_t value) noexcept {
    std::byte bytes[8];
    for (unsigned i = 0; i < 8; ++i) bytes[i] = std::byte(value >> (8 * i));
    Bytes(bytes);
  }
  void Bytes(std::span<const std::byte> bytes) noexcept {
    if (!ok_ || bytes.size() > output_.size() - size_) {
      ok_ = false;
      return;
    }
    std::memcpy(output_.data() + size_, bytes.data(), bytes.size());
    size_ += bytes.size();
  }
  void Text(std::string_view text) noexcept {
    U64(text.size());
    Bytes(std::as_bytes(std::span(text.data(), text.size())));
  }
  std::span<std::byte> tail() noexcept { return output_.subspan(size_); }
  void Advance(std::size_t bytes) noexcept {
    if (bytes > output_.size() - size_)
      ok_ = false;
    else
      size_ += bytes;
  }
  bool ok() const noexcept { return ok_; }
  std::size_t size() const noexcept { return size_; }

 private:
  std::span<std::byte> output_;
  std::size_t size_ = 0;
  bool ok_ = true;
};

class StateReader {
 public:
  explicit StateReader(std::span<const std::byte> input) noexcept
      : input_(input) {}
  std::uint64_t U64() noexcept {
    const auto bytes = Bytes(8);
    if (bytes.size() != 8) return 0;
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
      value |= std::uint64_t(std::to_integer<unsigned char>(bytes[i]))
               << (8 * i);
    return value;
  }
  std::span<const std::byte> Bytes(std::size_t size) noexcept {
    if (!ok_ || size > input_.size() - position_) {
      ok_ = false;
      return {};
    }
    auto result = input_.subspan(position_, size);
    position_ += size;
    return result;
  }
  bool Text(char* output, std::size_t capacity) noexcept {
    const auto size = U64();
    if (!ok_ || size >= capacity) {
      ok_ = false;
      return false;
    }
    const auto bytes = Bytes(size);
    if (!ok_ || std::memchr(bytes.data(), 0, bytes.size()) != nullptr) {
      ok_ = false;
      return false;
    }
    std::memcpy(output, bytes.data(), bytes.size());
    output[size] = '\0';
    return true;
  }
  bool ok() const noexcept { return ok_; }
  bool done() const noexcept { return ok_ && position_ == input_.size(); }

 private:
  std::span<const std::byte> input_;
  std::size_t position_ = 0;
  bool ok_ = true;
};

}  // namespace yquant::tradingd
#endif  // YQUANT_TRADINGD_STATE_CODEC_H_
