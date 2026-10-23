#ifndef YQUANT_BASE_LOGGING_H_
#define YQUANT_BASE_LOGGING_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <limits>
#include <string_view>
#include <type_traits>

#include "fmt/format.h"
#include "yquant/base/spsc_queue.h"
#include "yquant/base/status.h"

#define YQ_LOG_LEVEL_TRACE 0
#define YQ_LOG_LEVEL_DEBUG 1
#define YQ_LOG_LEVEL_INFO 2
#define YQ_LOG_LEVEL_WARNING 3
#define YQ_LOG_LEVEL_ERROR 4
#define YQ_LOG_LEVEL_CRITICAL 5
#define YQ_LOG_LEVEL_OFF 6

#ifndef YQ_LOG_COMPILED_LEVEL
#ifdef NDEBUG
#define YQ_LOG_COMPILED_LEVEL YQ_LOG_LEVEL_INFO
#else
#define YQ_LOG_COMPILED_LEVEL YQ_LOG_LEVEL_DEBUG
#endif
#endif

#if YQ_LOG_COMPILED_LEVEL < YQ_LOG_LEVEL_TRACE || \
    YQ_LOG_COMPILED_LEVEL > YQ_LOG_LEVEL_OFF
#error "Invalid YQ_LOG_COMPILED_LEVEL"
#endif

namespace yquant::base {

inline constexpr size_t kMaxLogBytes = 1024;
inline constexpr size_t kMaxLogInputBytes = 65536;
inline constexpr size_t kMaxLogFormatBytes = 1024;
inline constexpr size_t kMaxLogArguments = 16;
inline constexpr size_t kMaxLogNameBytes = 64;
inline constexpr size_t kMaxLogPathBytes = 4096;

enum class LogLevel : uint8_t {
  kTrace = YQ_LOG_LEVEL_TRACE,
  kDebug = YQ_LOG_LEVEL_DEBUG,
  kInfo = YQ_LOG_LEVEL_INFO,
  kWarning = YQ_LOG_LEVEL_WARNING,
  kError = YQ_LOG_LEVEL_ERROR,
  kCritical = YQ_LOG_LEVEL_CRITICAL,
  kOff = YQ_LOG_LEVEL_OFF,
};

struct LogCounters {
  uint64_t accepted_calls_total = 0;
  uint64_t enqueued_records_total = 0;
  uint64_t queue_dropped_records_total = 0;
  uint64_t input_rejected_records_total = 0;
  uint64_t reentrant_records_total = 0;
  uint64_t queue_errors_total = 0;
  uint64_t truncated_records_total = 0;
};

enum class LogErrorStage {
  kNone,
  kInitialize,
  kThread,
  kQueue,
  kOpen,
  kWrite,
  kSync,
  kClose,
  kRotate,
};

struct [[nodiscard]] ShutdownResult {
  Status status;
  LogErrorStage stage = LogErrorStage::kNone;
  size_t channel_index = std::numeric_limits<size_t>::max();
  uint64_t error_count = 0;
  LogCounters counters;
};

struct LogChannelError {
  Status status;
  LogErrorStage stage = LogErrorStage::kNone;
  uint64_t error_count = 0;
};

struct LogBackendStatus {
  Status status;
  LogErrorStage stage = LogErrorStage::kNone;
  uint64_t last_write_monotonic_ns = 0;
};

// Thread mode owns an in-process writer. Logd mode only maps producer queues.
enum class LogBackend { kThread, kLogd };

struct LogConfig {
  LogBackend backend = LogBackend::kThread;
  // Logd Shutdown waits at most this long across all channels for durable ACKs.
  uint64_t shutdown_timeout_ns = 5000000000ULL;
  size_t max_loggers = 16;
  size_t batch_bytes = 65536;
  uint64_t idle_sleep_ns = 1000000;
  void (*error_reporter)(const ShutdownResult&, void*) noexcept = nullptr;
  void* error_context = nullptr;
};

struct LoggerConfig {
  std::string_view name = "default";
  std::string_view file_path;
  uint64_t queue_capacity = 1048576;
  LogLevel minimum_level = LogLevel::kInfo;
  uint64_t min_interval_ns = 0;
  uint64_t max_file_bytes = 0;
  // Required in logd mode; logd must publish this queue before CreateLogger.
  // file_path and max_file_bytes are configured at the consumer in that mode.
  std::string_view queue_path;
};

class Logger;
class LogManager;

struct [[nodiscard]] CreateLoggerResult {
  Status status;
  Logger* logger = nullptr;
};

namespace internal {

struct LogManagerImpl;
struct SharedLogState;

// Called only from a failing constant evaluation, producing a compile error.
void InvalidLogFormat();

template <typename T>
inline constexpr bool kIsLogCharArray =
    std::is_array_v<std::remove_reference_t<T>> &&
    std::same_as<
        std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<T>>>,
        char>;

template <typename T>
using NormalizedLogArg =
    std::conditional_t<kIsLogCharArray<T>, std::string_view,
                       std::remove_cvref_t<T>>;

template <typename T>
inline constexpr bool kIsLogArg = [] {
  using Value = std::remove_cvref_t<T>;
  return kIsLogCharArray<T> || std::same_as<Value, std::string_view> ||
         std::same_as<Value, bool> || std::same_as<Value, char> ||
         std::same_as<Value, signed char> ||
         std::same_as<Value, unsigned char> || std::same_as<Value, short> ||
         std::same_as<Value, unsigned short> || std::same_as<Value, int> ||
         std::same_as<Value, unsigned int> || std::same_as<Value, long> ||
         std::same_as<Value, unsigned long> || std::same_as<Value, long long> ||
         std::same_as<Value, unsigned long long> ||
         std::same_as<Value, float> || std::same_as<Value, double>;
}();

template <typename T>
NormalizedLogArg<T> NormalizeLogArg(const T& value) noexcept {
  if constexpr (kIsLogCharArray<T>) {
    constexpr auto kSize = std::extent_v<std::remove_reference_t<T>>;
    return {value, kSize - (value[kSize - 1] == '\0' ? 1 : 0)};
  } else {
    return value;
  }
}

template <typename T>
bool ValidLogInput(const T& value) noexcept {
  if constexpr (kIsLogCharArray<T> ||
                std::same_as<std::remove_cvref_t<T>, std::string_view>) {
    return NormalizeLogArg(value).size() <= kMaxLogInputBytes;
  }
  return true;
}

// Copies of the iterator share a bounded writer. Escapes are emitted atomically
// so an overflowing escape cannot be followed by a later part of the message.
class LogBufferWriter {
 public:
  LogBufferWriter(const LogBufferWriter&) = default;
  LogBufferWriter& operator=(const LogBufferWriter&) = default;

  explicit LogBufferWriter(char* data) noexcept : data_(data) {}
  void Append(char value) noexcept;
  void MarkTruncated() noexcept { truncated_ = true; }
  [[nodiscard]] size_t remaining() const noexcept {
    return kMaxLogBytes - 1 - size_;
  }
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }
  [[nodiscard]] size_t Finish() noexcept;

 private:
  char* data_;
  size_t size_ = 0;
  bool truncated_ = false;
  bool full_ = false;
};

class LogOutputIterator {
 public:
  LogOutputIterator(const LogOutputIterator&) = default;
  LogOutputIterator& operator=(const LogOutputIterator&) = default;

  using iterator_category = std::output_iterator_tag;
  using value_type = void;
  using difference_type = ptrdiff_t;
  using pointer = void;
  using reference = void;

  explicit LogOutputIterator(LogBufferWriter& writer) noexcept
      : writer_(&writer) {}
  LogOutputIterator& operator*() noexcept { return *this; }
  LogOutputIterator& operator++() noexcept { return *this; }
  LogOutputIterator operator++(int) noexcept { return *this; }
  LogOutputIterator& operator=(char value) noexcept {
    writer_->Append(value);
    return *this;
  }

 private:
  LogBufferWriter* writer_;
};

struct AtomicLogCounters {
  std::atomic<uint64_t> accepted_calls_total{0};
  std::atomic<uint64_t> enqueued_records_total{0};
  std::atomic<uint64_t> queue_dropped_records_total{0};
  std::atomic<uint64_t> input_rejected_records_total{0};
  std::atomic<uint64_t> reentrant_records_total{0};
  std::atomic<uint64_t> queue_errors_total{0};
  std::atomic<uint64_t> truncated_records_total{0};
};

}  // namespace internal

template <typename... Args>
class LogFormat {
 public:
  LogFormat(const LogFormat&) = default;
  LogFormat& operator=(const LogFormat&) = default;

  template <size_t kSize>
  consteval LogFormat(const char (&text)[kSize]) : format_(text) {
    if (kSize > kMaxLogFormatBytes + 1 || text[kSize - 1] != '\0' ||
        sizeof...(Args) > kMaxLogArguments ||
        !(internal::kIsLogArg<Args> && ...)) {
      internal::InvalidLogFormat();
    }
    size_t arguments = 0;
    for (size_t i = 0; i + 1 < kSize; ++i) {
      if (text[i] == '{') {
        if (text[i + 1] == '{') {
          ++i;
        } else if (text[i + 1] == '}') {
          ++arguments;
          ++i;
        } else {
          internal::InvalidLogFormat();
        }
      } else if (text[i] == '}') {
        if (text[i + 1] != '}') {
          internal::InvalidLogFormat();
        }
        ++i;
      }
    }
    if (arguments != sizeof...(Args)) {
      internal::InvalidLogFormat();
    }
  }

  [[nodiscard]] fmt::format_string<internal::NormalizedLogArg<Args>...> format()
      const noexcept {
    return format_;
  }

 private:
  fmt::format_string<internal::NormalizedLogArg<Args>...> format_;
};

class Logger final {
 public:
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;
  Logger(Logger&&) = delete;
  Logger& operator=(Logger&&) = delete;

  void SetMinimumLevel(LogLevel level) noexcept;
  [[nodiscard]] LogLevel minimum_level() const noexcept;
  [[nodiscard]] std::string_view name() const noexcept;
  [[nodiscard]] LogCounters counters() const noexcept;
  [[nodiscard]] LogBackendStatus backend_status() const noexcept;

  // Macro support. The guard is acquired before evaluating log arguments and
  // spans their complete evaluation, formatting, publication and destruction.
  class Call final {
   public:
    Call() noexcept = default;
    ~Call() noexcept;
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;
    Call(Call&&) = delete;
    Call& operator=(Call&&) = delete;
    [[nodiscard]] bool active() const noexcept { return owner_ != nullptr; }

   private:
    friend class Logger;
    explicit Call(Logger* owner) noexcept : owner_(owner) {}
    Logger* owner_ = nullptr;
  };

  [[nodiscard]] Call BeginCall(LogLevel level) noexcept;

  template <typename... Args>
  void Write(Call& call, LogLevel level, const char* file, int line,
             LogFormat<std::type_identity_t<Args>...> format,
             Args&&... args) noexcept {
    if (call.owner_ != this) {
      return;
    }
    if (!(internal::ValidLogInput(args) && ...)) {
      counters_.input_rejected_records_total.fetch_add(
          1, std::memory_order_relaxed);
      return;
    }
    internal::LogBufferWriter writer(buffer_.data());
    WriteHeader(writer, level, file, line);
    const auto limit = writer.remaining();
    auto result =
        fmt::format_to_n(internal::LogOutputIterator(writer), limit,
                         format.format(), internal::NormalizeLogArg(args)...);
    if (result.size > limit) {
      writer.MarkTruncated();
    }
    const auto size = writer.Finish();
    Publish(size, writer.truncated());
  }

 private:
  friend class LogManager;
  friend struct internal::LogManagerImpl;

  Logger(std::string_view name, LogLevel level,
         uint64_t min_interval_ns) noexcept;
  ~Logger() noexcept = default;
  void WriteHeader(internal::LogBufferWriter& writer, LogLevel level,
                   const char* file, int line) noexcept;
  void Publish(size_t size, bool truncated) noexcept;

  SpscQueue producer_;
  // Borrowed mapping; stable until Shutdown, null for in-process producers.
  internal::SharedLogState* shared_state_ = nullptr;
  std::atomic<uint64_t> active_{0};
  std::atomic<uint64_t> queue_fault_{0};
  std::atomic<uint64_t> backend_error_{0};
  std::atomic<uint64_t> last_write_ns_{0};
  std::atomic<uint64_t> minimum_level_{static_cast<uint64_t>(LogLevel::kInfo)};
  internal::AtomicLogCounters counters_;
  std::array<char, kMaxLogBytes> buffer_{};
  std::array<char, kMaxLogNameBytes> name_{};
  size_t name_size_ = 0;
  uint64_t min_interval_ns_ = 0;
  uint64_t last_accepted_ns_ = 0;
  bool busy_ = false;
};

class LogManager final {
 public:
  LogManager() noexcept = default;
  ~LogManager() noexcept;
  LogManager(const LogManager&) = delete;
  LogManager& operator=(const LogManager&) = delete;
  LogManager(LogManager&&) = delete;
  LogManager& operator=(LogManager&&) = delete;

  // Management calls are serialized by the caller. Initialize starts a writer
  // thread only in kThread mode. Failure leaves the manager uninitialized.
  [[nodiscard]] Status Initialize(const LogConfig& config = {}) noexcept;
  // Success returns a manager-owned Logger borrowed until Shutdown. In logd
  // mode the queue must exist and match queue_capacity, and file_path and
  // max_file_bytes must be unset. Failure returns a null logger.
  [[nodiscard]] CreateLoggerResult CreateLogger(
      const LoggerConfig& config) noexcept;
  // Thread mode only: creates a shared queue and its file consumer for logd.
  // queue_path must not exist; parent directories must exist. One producer
  // session is permitted. Success publishes the queue atomically. Failure
  // rolls back resources; an existing queue file is never replaced.
  [[nodiscard]] Status CreateLogSink(const LoggerConfig& config) noexcept;
  // Join all producers before calling. Logd mode publishes end-of-stream and
  // waits for file sync/close, reporting ETIMEDOUT if the deadline expires.
  // Releases all Logger borrows; repeated calls return the saved result.
  [[nodiscard]] ShutdownResult Shutdown() noexcept;
  // Error slots remain valid until manager destruction; inspect after Shutdown.
  [[nodiscard]] LogChannelError channel_error(size_t index) const noexcept;

 private:
  [[nodiscard]] CreateLoggerResult CreateChannel(const LoggerConfig& config,
                                                 bool sink) noexcept;
  internal::LogManagerImpl* impl_ = nullptr;
};

}  // namespace yquant::base

#define YQ_DETAIL_LOG(logger, level, format, ...)                   \
  do {                                                              \
    auto& yq_log_logger_ = (logger);                                \
    auto yq_log_call_ = yq_log_logger_.BeginCall(level);            \
    if (yq_log_call_.active()) {                                    \
      yq_log_logger_.Write(yq_log_call_, level, __FILE__, __LINE__, \
                           format __VA_OPT__(, ) __VA_ARGS__);      \
    }                                                               \
  } while (false)

#if YQ_LOG_COMPILED_LEVEL <= YQ_LOG_LEVEL_TRACE
#define YQ_LOG_TRACE(logger, ...) \
  YQ_DETAIL_LOG(logger, ::yquant::base::LogLevel::kTrace, __VA_ARGS__)
#else
#define YQ_LOG_TRACE(...) ((void)0)
#endif
#if YQ_LOG_COMPILED_LEVEL <= YQ_LOG_LEVEL_DEBUG
#define YQ_LOG_DEBUG(logger, ...) \
  YQ_DETAIL_LOG(logger, ::yquant::base::LogLevel::kDebug, __VA_ARGS__)
#else
#define YQ_LOG_DEBUG(...) ((void)0)
#endif
#if YQ_LOG_COMPILED_LEVEL <= YQ_LOG_LEVEL_INFO
#define YQ_LOG_INFO(logger, ...) \
  YQ_DETAIL_LOG(logger, ::yquant::base::LogLevel::kInfo, __VA_ARGS__)
#else
#define YQ_LOG_INFO(...) ((void)0)
#endif
#if YQ_LOG_COMPILED_LEVEL <= YQ_LOG_LEVEL_WARNING
#define YQ_LOG_WARNING(logger, ...) \
  YQ_DETAIL_LOG(logger, ::yquant::base::LogLevel::kWarning, __VA_ARGS__)
#else
#define YQ_LOG_WARNING(...) ((void)0)
#endif
#if YQ_LOG_COMPILED_LEVEL <= YQ_LOG_LEVEL_ERROR
#define YQ_LOG_ERROR(logger, ...) \
  YQ_DETAIL_LOG(logger, ::yquant::base::LogLevel::kError, __VA_ARGS__)
#else
#define YQ_LOG_ERROR(...) ((void)0)
#endif
#if YQ_LOG_COMPILED_LEVEL <= YQ_LOG_LEVEL_CRITICAL
#define YQ_LOG_CRITICAL(logger, ...) \
  YQ_DETAIL_LOG(logger, ::yquant::base::LogLevel::kCritical, __VA_ARGS__)
#else
#define YQ_LOG_CRITICAL(...) ((void)0)
#endif

#endif  // YQUANT_BASE_LOGGING_H_
