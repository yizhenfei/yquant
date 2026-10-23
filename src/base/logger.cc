#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <string_view>
#include <system_error>

#include "yquant/base/logging.h"
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cassert>
#include <cstring>

#include "base/internal/logging_shared.h"

namespace yquant::base {
namespace {

static_assert(std::atomic<uint64_t>::is_always_lock_free);
constexpr std::string_view kTruncatedMarker = " [truncated]";

uint64_t ClockNanoseconds(clockid_t clock) noexcept {
  timespec value{};
  if (clock_gettime(clock, &value) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(value.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(value.tv_nsec);
}

uint64_t CurrentThreadId() noexcept {
#if defined(__APPLE__)
  uint64_t id = 0;
  (void)pthread_threadid_np(nullptr, &id);
  return id;
#else
  return static_cast<uint64_t>(syscall(SYS_gettid));
#endif
}

std::string_view LevelName(LogLevel level) noexcept {
  constexpr std::array<std::string_view, 6> kNames = {
      "TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"};
  const auto index = static_cast<size_t>(level);
  return index < kNames.size() ? kNames[index] : std::string_view("INVALID");
}

// Keep a complete prefix of UTF-8 characters and escaped output tokens.
size_t CompletePrefix(const char* data, size_t limit) noexcept {
  size_t position = 0;
  while (position < limit) {
    const auto value = static_cast<unsigned char>(data[position]);
    size_t bytes = 1;
    if (value == '\\') {
      bytes = position + 1 < limit && data[position + 1] == 'x' ? 4 : 2;
    } else if ((value & 0xe0) == 0xc0) {
      bytes = 2;
    } else if ((value & 0xf0) == 0xe0) {
      bytes = 3;
    } else if ((value & 0xf8) == 0xf0) {
      bytes = 4;
    }
    if (bytes > limit - position) {
      break;
    }
    position += bytes;
  }
  return position;
}

}  // namespace

namespace internal {

void LogBufferWriter::Append(char value) noexcept {
  if (full_) {
    return;
  }
  char escaped[4];
  size_t count = 1;
  escaped[0] = value;
  const auto byte = static_cast<unsigned char>(value);
  if (value == '\n' || value == '\r' || value == '\\') {
    escaped[0] = '\\';
    escaped[1] = value == '\n' ? 'n' : value == '\r' ? 'r' : '\\';
    count = 2;
  } else if (byte < 0x20 || byte == 0x7f) {
    constexpr char kHex[] = "0123456789abcdef";
    escaped[0] = '\\';
    escaped[1] = 'x';
    escaped[2] = kHex[byte >> 4];
    escaped[3] = kHex[byte & 0x0f];
    count = 4;
  }
  if (count > remaining()) {
    full_ = true;
    truncated_ = true;
    return;
  }
  std::memcpy(data_ + size_, escaped, count);
  size_ += count;
}

size_t LogBufferWriter::Finish() noexcept {
  if (truncated_) {
    const auto limit =
        std::min(size_, kMaxLogBytes - 1 - kTruncatedMarker.size());
    size_ = CompletePrefix(data_, limit);
    std::memcpy(data_ + size_, kTruncatedMarker.data(),
                kTruncatedMarker.size());
    size_ += kTruncatedMarker.size();
  }
  data_[size_++] = '\n';
  return size_;
}

}  // namespace internal

Logger::Logger(std::string_view name, LogLevel level,
               uint64_t min_interval_ns) noexcept
    : minimum_level_(static_cast<uint64_t>(level)),
      name_size_(name.size()),
      min_interval_ns_(min_interval_ns) {
  assert(name.size() <= name_.size());
  std::memcpy(name_.data(), name.data(), name.size());
}

void Logger::SetMinimumLevel(LogLevel level) noexcept {
  minimum_level_.store(static_cast<uint64_t>(level), std::memory_order_relaxed);
}

LogLevel Logger::minimum_level() const noexcept {
  return static_cast<LogLevel>(minimum_level_.load(std::memory_order_relaxed));
}

std::string_view Logger::name() const noexcept {
  return {name_.data(), name_size_};
}

LogCounters Logger::counters() const noexcept {
  return {
      counters_.accepted_calls_total.load(std::memory_order_relaxed),
      counters_.enqueued_records_total.load(std::memory_order_relaxed),
      counters_.queue_dropped_records_total.load(std::memory_order_relaxed),
      counters_.input_rejected_records_total.load(std::memory_order_relaxed),
      counters_.reentrant_records_total.load(std::memory_order_relaxed),
      counters_.queue_errors_total.load(std::memory_order_relaxed),
      counters_.truncated_records_total.load(std::memory_order_relaxed)};
}

LogBackendStatus Logger::backend_status() const noexcept {
  const auto error =
      shared_state_ != nullptr
          ? shared_state_->backend_error.load(std::memory_order_acquire)
          : backend_error_.load(std::memory_order_acquire);
  const auto native_error = static_cast<int>(error >> 16);
  const auto system_error =
      native_error == 0
          ? std::error_code{}
          : std::error_code(native_error, std::generic_category());
  return {{static_cast<ErrorCode>(error & 0xff), system_error},
          static_cast<LogErrorStage>((error >> 8) & 0xff),
          shared_state_ != nullptr
              ? shared_state_->last_write_ns.load(std::memory_order_relaxed)
              : last_write_ns_.load(std::memory_order_relaxed)};
}

Logger::Call::~Call() noexcept {
  if (owner_ != nullptr) {
    owner_->busy_ = false;
  }
}

Logger::Call Logger::BeginCall(LogLevel level) noexcept {
  if (level > LogLevel::kCritical || level < minimum_level()) {
    return {};
  }
  const auto now =
      min_interval_ns_ == 0 ? 0 : ClockNanoseconds(CLOCK_MONOTONIC);
  if (min_interval_ns_ != 0 && last_accepted_ns_ != 0 &&
      now - last_accepted_ns_ < min_interval_ns_) {
    return {};
  }
  counters_.accepted_calls_total.fetch_add(1, std::memory_order_relaxed);
  if (busy_) {
    counters_.reentrant_records_total.fetch_add(1, std::memory_order_relaxed);
    return {};
  }
  if (active_.load(std::memory_order_acquire) == 0 ||
      (shared_state_ != nullptr &&
       (shared_state_->backend_error.load(std::memory_order_acquire) != 0 ||
        shared_state_->consumer_done.load(std::memory_order_acquire) != 0))) {
    counters_.queue_errors_total.fetch_add(1, std::memory_order_relaxed);
    return {};
  }
  const auto space = producer_.CheckWrite(1);
  if (space != SpscStatus::kSuccess) {
    if (space == SpscStatus::kFull) {
      counters_.queue_dropped_records_total.fetch_add(
          1, std::memory_order_relaxed);
    } else {
      counters_.queue_errors_total.fetch_add(1, std::memory_order_relaxed);
      queue_fault_.store(1, std::memory_order_release);
      if (shared_state_ != nullptr) {
        shared_state_->queue_fault.store(1, std::memory_order_release);
      }
      active_.store(0, std::memory_order_release);
    }
    return {};
  }
  last_accepted_ns_ = now;
  busy_ = true;
  return Call(this);
}

void Logger::WriteHeader(internal::LogBufferWriter& writer, LogLevel level,
                         const char* file, int line) noexcept {
  // Source locations are compile-time strings supplied by the macro. Only the
  // final text is queued; clocks, thread identity and arguments stay here.
  const auto limit = writer.remaining();
  auto result =
      fmt::format_to_n(internal::LogOutputIterator(writer), limit,
                       "[{}] [{}] [tid={}] [{}] {}:{} ",
                       ClockNanoseconds(CLOCK_REALTIME), LevelName(level),
                       CurrentThreadId(), name(), std::string_view(file), line);
  if (result.size > limit) {
    writer.MarkTruncated();
  }
}

void Logger::Publish(size_t size, bool truncated) noexcept {
  if (active_.load(std::memory_order_acquire) == 0 ||
      (shared_state_ != nullptr &&
       (shared_state_->backend_error.load(std::memory_order_acquire) != 0 ||
        shared_state_->consumer_done.load(std::memory_order_acquire) != 0))) {
    counters_.queue_errors_total.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto payload = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(buffer_.data()), size);
  const auto status = producer_.TryWrite(payload);
  if (status == SpscStatus::kSuccess) {
    counters_.enqueued_records_total.fetch_add(1, std::memory_order_relaxed);
    if (truncated) {
      counters_.truncated_records_total.fetch_add(1, std::memory_order_relaxed);
    }
  } else if (status == SpscStatus::kFull) {
    counters_.queue_dropped_records_total.fetch_add(1,
                                                    std::memory_order_relaxed);
  } else {
    counters_.queue_errors_total.fetch_add(1, std::memory_order_relaxed);
    queue_fault_.store(1, std::memory_order_release);
    if (shared_state_ != nullptr) {
      shared_state_->queue_fault.store(1, std::memory_order_release);
    }
    active_.store(0, std::memory_order_release);
  }
}

}  // namespace yquant::base
