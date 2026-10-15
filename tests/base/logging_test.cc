// Runtime coverage retains every level; compile-check fixtures cover stripping.
#ifdef YQ_LOG_COMPILED_LEVEL
#undef YQ_LOG_COMPILED_LEVEL
#endif
#define YQ_LOG_COMPILED_LEVEL YQ_LOG_LEVEL_TRACE

#include "yquant/base/logging.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "base/internal/logging_backend.h"
#include "base/internal/spsc_queue_layout.h"
#include "gtest/gtest.h"

namespace yquant::base {
namespace {

class LogDirectory {
 public:
  LogDirectory() {
    std::array<char, 64> name{};
    std::strcpy(name.data(), "/private/tmp/yquant-logging-XXXXXX");
#if defined(__linux__)
    std::strcpy(name.data(), "/tmp/yquant-logging-XXXXXX");
#endif
    if (char* path = mkdtemp(name.data()); path != nullptr) path_ = path;
  }

  ~LogDirectory() {
    for (const auto& filename : Files()) unlink(filename.c_str());
    if (!path_.empty()) rmdir(path_.c_str());
  }

  bool valid() const { return !path_.empty(); }
  const std::string& path() const { return path_; }
  std::string File(std::string_view filename) const {
    return path_ + "/" + std::string(filename);
  }

  std::vector<std::string> Files() const {
    std::vector<std::string> files;
    DIR* directory = opendir(path_.c_str());
    if (directory == nullptr) return files;
    while (dirent* entry = readdir(directory)) {
      if (std::strcmp(entry->d_name, ".") == 0 ||
          std::strcmp(entry->d_name, "..") == 0) {
        continue;
      }
      files.push_back(File(entry->d_name));
    }
    closedir(directory);
    std::sort(files.begin(), files.end());
    return files;
  }

 private:
  std::string path_;
};

std::string ReadFile(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return {};
  std::string text;
  std::array<char, 4096> buffer{};
  while (const auto count = std::fread(buffer.data(), 1, buffer.size(), file)) {
    text.append(buffer.data(), count);
  }
  std::fclose(file);
  return text;
}

std::size_t Occurrences(std::string_view haystack, std::string_view needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = haystack.find(needle, offset)) != std::string_view::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

template <typename Predicate>
bool WaitFor(Predicate&& predicate) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    sched_yield();
  }
  return true;
}

bool IsValidUtf8(std::string_view text) {
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<unsigned char>(text[index]);
    std::size_t continuation = 0;
    if (first <= 0x7f) {
      ++index;
      continue;
    }
    if (first >= 0xc2 && first <= 0xdf) continuation = 1;
    if (first >= 0xe0 && first <= 0xef) continuation = 2;
    if (first >= 0xf0 && first <= 0xf4) continuation = 3;
    if (continuation == 0 || index + continuation >= text.size()) return false;
    for (std::size_t next = 1; next <= continuation; ++next) {
      const auto value = static_cast<unsigned char>(text[index + next]);
      if ((value & 0xc0) != 0x80) return false;
    }
    const auto second = static_cast<unsigned char>(text[index + 1]);
    if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
        (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) {
      return false;
    }
    index += continuation + 1;
  }
  return true;
}

struct IoControl {
  std::atomic<bool> pause{false};
  std::atomic<bool> paused{false};
  std::atomic<bool> fail_write{false};
  std::atomic<bool> fail_sync{false};
  std::atomic<bool> fail_close{false};
  std::atomic<bool> interrupt_next_write{false};
  std::atomic<std::size_t> write_calls{0};
  std::atomic<std::size_t> sync_calls{0};
  std::atomic<std::size_t> close_calls{0};
  std::size_t max_write = 0;
  int fault_fd = -1;
  Logger* first_logger = nullptr;
  std::span<std::byte> first_queue_memory;

  static void OnQueueReady(void* opaque, Logger* logger,
                           std::span<std::byte> memory) noexcept {
    auto& control = *static_cast<IoControl*>(opaque);
    if (control.first_logger == nullptr) {
      control.first_logger = logger;
      control.first_queue_memory = memory;
    }
  }

  static void BeforePoll(void* opaque) noexcept {
    auto& control = *static_cast<IoControl*>(opaque);
    while (control.pause.load(std::memory_order_acquire)) {
      control.paused.store(true, std::memory_order_release);
      sched_yield();
    }
    control.paused.store(false, std::memory_order_release);
  }

  static int Open(void* opaque, const char* path, int flags,
                  mode_t mode) noexcept {
    auto& control = *static_cast<IoControl*>(opaque);
    int fd = ::open(path, flags, mode);
    if (std::strstr(path, "fault.log") != nullptr) control.fault_fd = fd;
    return fd;
  }

  static ssize_t Write(void* opaque, int fd, const void* data,
                       std::size_t size) noexcept {
    auto& control = *static_cast<IoControl*>(opaque);
    control.write_calls.fetch_add(1, std::memory_order_relaxed);
    if (control.interrupt_next_write.exchange(false)) {
      errno = EINTR;
      return -1;
    }
    if (fd == control.fault_fd && control.fail_write.load()) {
      errno = ENOSPC;
      return -1;
    }
    if (control.max_write != 0) size = std::min(size, control.max_write);
    return ::write(fd, data, size);
  }

  static int Sync(void* opaque, int fd) noexcept {
    auto& control = *static_cast<IoControl*>(opaque);
    control.sync_calls.fetch_add(1, std::memory_order_relaxed);
    if (fd == control.fault_fd && control.fail_sync.load()) {
      errno = EIO;
      return -1;
    }
    return fsync(fd);
  }

  static int Close(void* opaque, int fd) noexcept {
    auto& control = *static_cast<IoControl*>(opaque);
    control.close_calls.fetch_add(1, std::memory_order_relaxed);
    const int result = ::close(fd);
    if (fd == control.fault_fd && control.fail_close.load()) {
      errno = EIO;
      return -1;
    }
    return result;
  }

  internal::LogBackendHooks Hooks() {
    internal::LogBackendHooks hooks;
    hooks.context = this;
    hooks.before_poll = BeforePoll;
    hooks.on_queue_ready = OnQueueReady;
    hooks.open = Open;
    hooks.write = Write;
    hooks.sync = Sync;
    hooks.close = Close;
    return hooks;
  }

  bool Pause() {
    pause.store(true, std::memory_order_release);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!paused.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        pause.store(false, std::memory_order_release);
        return false;
      }
      sched_yield();
    }
    return true;
  }

  void Resume() { pause.store(false, std::memory_order_release); }
};

class ScopedHooks {
 public:
  explicit ScopedHooks(IoControl& control)
      : control_(control), hooks_(control.Hooks()) {
    internal::SetLogBackendHooksForTesting(&hooks_);
  }
  ~ScopedHooks() {
    control_.Resume();
    internal::SetLogBackendHooksForTesting(nullptr);
  }

 private:
  IoControl& control_;
  internal::LogBackendHooks hooks_;
};

Logger* CreateLogger(LogManager& manager, const LogDirectory& directory,
                     std::string_view name = "test",
                     LogLevel minimum_level = LogLevel::kTrace,
                     std::uint64_t queue_capacity = 1U << 20,
                     std::uint64_t max_file_bytes = 0) {
  const std::string path = directory.File(std::string(name) + ".log");
  LoggerConfig config;
  config.name = name;
  config.file_path = path;
  config.minimum_level = minimum_level;
  config.queue_capacity = queue_capacity;
  config.max_file_bytes = max_file_bytes;
  const auto created = manager.CreateLogger(config);
  EXPECT_TRUE(created.status.ok());
  EXPECT_NE(created.logger, nullptr);
  return created.logger;
}

void CheckConservation(const LogCounters& counters) {
  EXPECT_EQ(counters.accepted_calls_total,
            counters.enqueued_records_total +
                counters.queue_dropped_records_total +
                counters.input_rejected_records_total +
                counters.reentrant_records_total + counters.queue_errors_total);
  EXPECT_LE(counters.truncated_records_total, counters.enqueued_records_total);
}

TEST(LoggingLifecycleTest, EmptyAndUninitializedManagersAreSafe) {
  LogManager manager;
  const auto first = manager.Shutdown();
  const auto second = manager.Shutdown();
  EXPECT_EQ(first.status.ok(), second.status.ok());
  EXPECT_EQ(first.error_count, second.error_count);
  EXPECT_EQ(first.counters.enqueued_records_total, 0U);
  EXPECT_FALSE(manager.CreateLogger({}).status.ok());
}

TEST(LoggingLifecycleTest, InvalidInitializationCanBeRetried) {
  LogManager manager;
  LogConfig invalid;
  invalid.max_loggers = 0;
  EXPECT_FALSE(manager.Initialize(invalid).ok());
  EXPECT_TRUE(manager.Initialize({}).ok());
  EXPECT_FALSE(manager.Initialize({}).ok());
  EXPECT_TRUE(manager.Shutdown().status.ok());
}

TEST(LoggingLifecycleTest, FailedCreationRollsBackItsSlotAndResources) {
  LogDirectory directory;
  ASSERT_TRUE(directory.valid());
  LogManager manager;
  LogConfig config;
  config.max_loggers = 1;
  ASSERT_TRUE(manager.Initialize(config).ok());
  LoggerConfig invalid;
  invalid.name = "first";
  const std::string bad_path = directory.File("missing/first.log");
  invalid.file_path = bad_path;
  const auto failed = manager.CreateLogger(invalid);
  EXPECT_FALSE(failed.status.ok());
  EXPECT_EQ(failed.logger, nullptr);
  Logger* logger = CreateLogger(manager, directory, "first");
  ASSERT_NE(logger, nullptr);
  EXPECT_EQ(logger->name(), "first");
  EXPECT_FALSE(manager.CreateLogger(invalid).status.ok());
  YQ_LOG_INFO(*logger, "created-after-failure");
  const auto result = manager.Shutdown();
  EXPECT_TRUE(result.status.ok());
  EXPECT_NE(ReadFile(directory.File("first.log")).find("created-after-failure"),
            std::string::npos);
}

TEST(LoggingLifecycleTest, RejectsInvalidLoggerConfiguration) {
  LogDirectory directory;
  ASSERT_TRUE(directory.valid());
  LogManager manager;
  ASSERT_TRUE(manager.Initialize({}).ok());
  LoggerConfig config;
  const std::string path = directory.File("invalid.log");
  config.name = "valid";
  config.file_path = path;
  config.queue_capacity = 1000;
  EXPECT_FALSE(manager.CreateLogger(config).status.ok());
  config.queue_capacity = 1024;
  EXPECT_FALSE(manager.CreateLogger(config).status.ok());
  config.queue_capacity = 1U << 20;
  config.name = "line\nbreak";
  EXPECT_FALSE(manager.CreateLogger(config).status.ok());
  config.name = "valid";
  config.file_path = {};
  EXPECT_FALSE(manager.CreateLogger(config).status.ok());
  const std::string longest_name(kMaxLogNameBytes, 'n');
  config.name = longest_name;
  config.file_path = path;
  const auto longest = manager.CreateLogger(config);
  ASSERT_TRUE(longest.status.ok());
  ASSERT_NE(longest.logger, nullptr);
  EXPECT_EQ(longest.logger->name(), longest_name);
  EXPECT_TRUE(manager.Shutdown().status.ok());
}

TEST(LoggingLifecycleTest, RejectsFileAliasesWithoutConsumingRegistrySlots) {
  LogDirectory directory;
  ASSERT_TRUE(directory.valid());
  LogManager manager;
  LogConfig config;
  config.max_loggers = 2;
  ASSERT_TRUE(manager.Initialize(config).ok());
  Logger* original = CreateLogger(manager, directory, "original");
  ASSERT_NE(original, nullptr);
  const std::string original_path = directory.File("original.log");
  const std::string symlink_path = directory.File("symlink.log");
  const std::string hardlink_path = directory.File("hardlink.log");
  ASSERT_EQ(symlink(original_path.c_str(), symlink_path.c_str()), 0);
  ASSERT_EQ(link(original_path.c_str(), hardlink_path.c_str()), 0);
  const std::array aliases = {directory.File("./original.log"), symlink_path,
                              hardlink_path};
  for (const auto& path : aliases) {
    SCOPED_TRACE(path);
    LoggerConfig duplicate;
    duplicate.name = "alias";
    duplicate.file_path = path;
    const auto rejected = manager.CreateLogger(duplicate);
    EXPECT_FALSE(rejected.status.ok());
    EXPECT_EQ(rejected.logger, nullptr);
    YQ_LOG_INFO(*original, "original-after-rejected-alias");
  }
  Logger* independent = CreateLogger(manager, directory, "independent");
  ASSERT_NE(independent, nullptr);
  YQ_LOG_INFO(*independent, "independent-after-rejected-aliases");
  EXPECT_TRUE(manager.Shutdown().status.ok());
  EXPECT_EQ(
      Occurrences(ReadFile(original_path), "original-after-rejected-alias"),
      aliases.size());
  EXPECT_NE(ReadFile(directory.File("independent.log"))
                .find("independent-after-rejected-aliases"),
            std::string::npos);
}

TEST(LoggingLifecycleTest, RejectsFifoAndDeviceWithoutConsumingRegistrySlots) {
  LogDirectory directory;
  ASSERT_TRUE(directory.valid());
  const std::string fifo_path = directory.File("pipe");
  ASSERT_EQ(mkfifo(fifo_path.c_str(), 0600), 0);
  LogManager manager;
  LogConfig config;
  config.max_loggers = 1;
  ASSERT_TRUE(manager.Initialize(config).ok());
  for (const auto path :
       {std::string_view(fifo_path), std::string_view("/dev/null")}) {
    SCOPED_TRACE(path);
    LoggerConfig invalid;
    invalid.name = "nonregular";
    invalid.file_path = path;
    const auto rejected = manager.CreateLogger(invalid);
    EXPECT_FALSE(rejected.status.ok());
    EXPECT_EQ(rejected.logger, nullptr);
  }
  Logger* logger = CreateLogger(manager, directory);
  ASSERT_NE(logger, nullptr);
  YQ_LOG_INFO(*logger, "regular-after-rejected-special-files");
  EXPECT_TRUE(manager.Shutdown().status.ok());
  EXPECT_NE(ReadFile(directory.File("test.log"))
                .find("regular-after-rejected-special-files"),
            std::string::npos);
}

TEST(LoggingTest, AllLevelsAndRuntimeFilteringAvoidArgumentEvaluation) {
  LogDirectory directory;
  ASSERT_TRUE(directory.valid());
  LogManager manager;
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* logger = CreateLogger(manager, directory);
  ASSERT_NE(logger, nullptr);
  YQ_LOG_TRACE(*logger, "level-trace");
  YQ_LOG_DEBUG(*logger, "level-debug");
  YQ_LOG_INFO(*logger, "level-info");
  YQ_LOG_WARNING(*logger, "level-warning");
  YQ_LOG_ERROR(*logger, "level-error");
  YQ_LOG_CRITICAL(*logger, "level-critical");
  int evaluated = 0;
  logger->SetMinimumLevel(LogLevel::kError);
  EXPECT_EQ(logger->minimum_level(), LogLevel::kError);
  YQ_LOG_INFO(*logger, "filtered={}", ++evaluated);
  YQ_LOG_ERROR(*logger, "retained={}", ++evaluated);
  logger->SetMinimumLevel(LogLevel::kOff);
  YQ_LOG_CRITICAL(*logger, "off={}", ++evaluated);
  EXPECT_EQ(evaluated, 1);
  const auto counts = logger->counters();
  EXPECT_EQ(counts.accepted_calls_total, 7U);
  EXPECT_EQ(counts.enqueued_records_total, 7U);
  CheckConservation(counts);
  EXPECT_TRUE(manager.Shutdown().status.ok());
  const auto text = ReadFile(directory.File("test.log"));
  for (const auto level :
       {"trace", "debug", "info", "warning", "error", "critical"}) {
    EXPECT_NE(text.find(std::string("level-") + level), std::string::npos);
  }
  EXPECT_EQ(Occurrences(text, "\n"), 7U);
  EXPECT_EQ(text.find("filtered="), std::string::npos);
  EXPECT_EQ(text.find("off="), std::string::npos);
}

TEST(LoggingTest, EvaluatesLoggerExpressionExactlyOnce) {
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* logger = CreateLogger(manager, directory);
  ASSERT_NE(logger, nullptr);
  int calls = 0;
  auto get_logger = [&]() -> Logger& {
    ++calls;
    return *logger;
  };
  YQ_LOG_INFO(get_logger(), "single-evaluation");
  logger->SetMinimumLevel(LogLevel::kOff);
  YQ_LOG_INFO(get_logger(), "single-filtered");
  EXPECT_EQ(calls, 2);
  EXPECT_TRUE(manager.Shutdown().status.ok());
}

TEST(LoggingTest, RateLimitedCallsDoNotEvaluateArgumentsOrCountAsLoss) {
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  const std::string path = directory.File("limited.log");
  LoggerConfig config;
  config.name = "limited";
  config.file_path = path;
  config.min_interval_ns = 3600ULL * 1000000000ULL;
  const auto result = manager.CreateLogger(config);
  ASSERT_TRUE(result.status.ok());
  ASSERT_NE(result.logger, nullptr);
  int evaluated = 0;
  YQ_LOG_INFO(*result.logger, "first={}", ++evaluated);
  YQ_LOG_INFO(*result.logger, "limited={}", ++evaluated);
  EXPECT_EQ(evaluated, 1);
  const auto counts = result.logger->counters();
  EXPECT_EQ(counts.accepted_calls_total, 1U);
  EXPECT_EQ(counts.enqueued_records_total, 1U);
  CheckConservation(counts);
  EXPECT_TRUE(manager.Shutdown().status.ok());
  const auto text = ReadFile(path);
  EXPECT_NE(text.find("first=1"), std::string::npos);
  EXPECT_EQ(text.find("limited="), std::string::npos);
}

int LogNested(Logger& logger, int& evaluated) {
  YQ_LOG_INFO(logger, "inner={}", ++evaluated);
  return 41;
}

TEST(LoggingTest, SameLoggerReentryIsDroppedBeforeEvaluatingItsArguments) {
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* logger = CreateLogger(manager, directory);
  ASSERT_NE(logger, nullptr);
  int inner_evaluated = 0;
  YQ_LOG_INFO(*logger, "outer={}", LogNested(*logger, inner_evaluated));
  YQ_LOG_INFO(*logger, "after-reentry");
  EXPECT_EQ(inner_evaluated, 0);
  const auto counts = logger->counters();
  EXPECT_EQ(counts.reentrant_records_total, 1U);
  EXPECT_EQ(counts.enqueued_records_total, 2U);
  CheckConservation(counts);
  EXPECT_TRUE(manager.Shutdown().status.ok());
  const auto text = ReadFile(directory.File("test.log"));
  EXPECT_NE(text.find("outer=41"), std::string::npos);
  EXPECT_NE(text.find("after-reentry"), std::string::npos);
  EXPECT_EQ(text.find("inner="), std::string::npos);
}

struct TemporaryLogText {
  Logger* logger;
  ~TemporaryLogText() { YQ_LOG_INFO(*logger, "temporary-destructor-inner"); }
  std::string_view view() const { return "temporary-text"; }
};

TEST(LoggingTest, ReentryGuardRemainsActiveDuringArgumentDestruction) {
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* logger = CreateLogger(manager, directory);
  ASSERT_NE(logger, nullptr);
  YQ_LOG_INFO(*logger, "outer={}", TemporaryLogText{logger}.view());
  YQ_LOG_INFO(*logger, "after-temporary-destruction");
  const auto counts = logger->counters();
  EXPECT_EQ(counts.reentrant_records_total, 1U);
  EXPECT_EQ(counts.enqueued_records_total, 2U);
  CheckConservation(counts);
  EXPECT_TRUE(manager.Shutdown().status.ok());
  const auto text = ReadFile(directory.File("test.log"));
  EXPECT_NE(text.find("outer=temporary-text"), std::string::npos);
  EXPECT_NE(text.find("after-temporary-destruction"), std::string::npos);
  EXPECT_EQ(text.find("temporary-destructor-inner"), std::string::npos);
}

TEST(LoggingTest, DifferentLoggersCanNestWithIndependentBuffers) {
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* first = CreateLogger(manager, directory, "first");
  Logger* second = CreateLogger(manager, directory, "second");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  int evaluated = 0;
  YQ_LOG_INFO(*first, "outer={}", LogNested(*second, evaluated));
  EXPECT_EQ(evaluated, 1);
  EXPECT_EQ(first->counters().reentrant_records_total, 0U);
  EXPECT_EQ(second->counters().reentrant_records_total, 0U);
  EXPECT_TRUE(manager.Shutdown().status.ok());
  EXPECT_NE(ReadFile(directory.File("first.log")).find("outer=41"),
            std::string::npos);
  EXPECT_NE(ReadFile(directory.File("second.log")).find("inner=1"),
            std::string::npos);
}

TEST(LoggingTest, OversizedInputIsRejectedAndTheBufferBecomesReusable) {
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* logger = CreateLogger(manager, directory);
  ASSERT_NE(logger, nullptr);
  std::string too_large(kMaxLogInputBytes + 1, 'x');
  YQ_LOG_INFO(*logger, "{}", std::string_view(too_large));
  YQ_LOG_INFO(*logger, "after-input-rejection");
  const auto counts = logger->counters();
  EXPECT_EQ(counts.input_rejected_records_total, 1U);
  EXPECT_EQ(counts.enqueued_records_total, 1U);
  CheckConservation(counts);
  EXPECT_TRUE(manager.Shutdown().status.ok());
  const auto text = ReadFile(directory.File("test.log"));
  EXPECT_NE(text.find("after-input-rejection"), std::string::npos);
  EXPECT_EQ(Occurrences(text, "\n"), 1U);
}

TEST(LoggingTest, TruncationPreservesUtf8PrefixAndNewline) {
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* logger = CreateLogger(manager, directory);
  ASSERT_NE(logger, nullptr);
  const std::string chinese = "汉字🦊";
  std::string long_text;
  while (long_text.size() < 4 * kMaxLogBytes) long_text += chinese;
  for (std::size_t padding = 0; padding < 8; ++padding) {
    const std::string ascii_padding(padding, 'a');
    YQ_LOG_INFO(*logger, "prefix:{}{}", std::string_view(ascii_padding),
                std::string_view(long_text));
  }
  const auto counts = logger->counters();
  EXPECT_EQ(counts.enqueued_records_total, 8U);
  EXPECT_EQ(counts.truncated_records_total, 8U);
  EXPECT_EQ(counts.queue_dropped_records_total, 0U);
  CheckConservation(counts);
  EXPECT_TRUE(manager.Shutdown().status.ok());
  const auto text = ReadFile(directory.File("test.log"));
  std::size_t offset = 0;
  for (std::size_t padding = 0; padding < 8; ++padding) {
    const auto newline = text.find('\n', offset);
    ASSERT_NE(newline, std::string::npos);
    const std::string_view line(text.data() + offset, newline - offset + 1);
    EXPECT_LE(line.size(), kMaxLogBytes);
    EXPECT_TRUE(line.ends_with(" [truncated]\n"));
    EXPECT_TRUE(IsValidUtf8(line));
    const auto prefix = line.find("prefix:");
    ASSERT_NE(prefix, std::string::npos);
    const auto content = line.substr(prefix + 7, line.size() - prefix - 7 - 13);
    const auto original = std::string(padding, 'a') + long_text;
    EXPECT_TRUE(original.starts_with(content));
    offset = newline + 1;
  }
  EXPECT_EQ(offset, text.size());
}

TEST(LoggingTest, TextNewlinesAreEscapedIntoOnePhysicalLogLine) {
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* logger = CreateLogger(manager, directory);
  ASSERT_NE(logger, nullptr);
  YQ_LOG_INFO(*logger, "line={} {{escaped}}", std::string_view("a\nb\rc"));
  EXPECT_TRUE(manager.Shutdown().status.ok());
  const auto text = ReadFile(directory.File("test.log"));
  EXPECT_EQ(Occurrences(text, "\n"), 1U);
  EXPECT_NE(text.find("a\\nb\\rc"), std::string::npos);
  EXPECT_NE(text.find("{escaped}"), std::string::npos);
}

TEST(LoggingTest, BoundedTextSupportsEmptyAndNonTerminatedInput) {
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* logger = CreateLogger(manager, directory);
  ASSERT_NE(logger, nullptr);
  const std::string_view empty;
  const char bounded[] = {'a', 'b', 'c'};
  const char embedded_nul[] = {'x', '\0', 'y'};
  YQ_LOG_INFO(*logger, "empty=[{}] bounded=[{}] nul=[{}]", empty, bounded,
              std::string_view(embedded_nul, sizeof(embedded_nul)));
  const auto counts = logger->counters();
  EXPECT_EQ(counts.enqueued_records_total, 1U);
  EXPECT_EQ(counts.input_rejected_records_total, 0U);
  EXPECT_EQ(counts.truncated_records_total, 0U);
  CheckConservation(counts);
  EXPECT_TRUE(manager.Shutdown().status.ok());
  const auto text = ReadFile(directory.File("test.log"));
  EXPECT_NE(text.find("empty=[] bounded=[abc] nul=[x\\x00y]\n"),
            std::string::npos);
  EXPECT_EQ(Occurrences(text, "\n"), 1U);
}

TEST(LoggingTest, QueuedTextOwnsBytesAfterCallerInputChanges) {
  IoControl control;
  ScopedHooks hooks(control);
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* logger = CreateLogger(manager, directory);
  ASSERT_NE(logger, nullptr);
  ASSERT_TRUE(control.Pause());
  std::string source = "original-caller-text";
  YQ_LOG_INFO(*logger, "snapshot={}", std::string_view(source));
  source.assign(source.size(), 'z');
  for (int index = 0; index < 20; ++index) {
    YQ_LOG_INFO(*logger, "buffer-reuse={}", index);
  }
  control.Resume();
  EXPECT_TRUE(manager.Shutdown().status.ok());
  const auto text = ReadFile(directory.File("test.log"));
  EXPECT_NE(text.find("snapshot=original-caller-text"), std::string::npos);
  EXPECT_EQ(text.find("snapshot=zzzz"), std::string::npos);
  EXPECT_EQ(Occurrences(text, "buffer-reuse="), 20U);
}

TEST(LoggingTest, FullQueueDropsWithoutEvaluatingFurtherArguments) {
  IoControl control;
  ScopedHooks hooks(control);
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* logger =
      CreateLogger(manager, directory, "test", LogLevel::kTrace, 2048);
  ASSERT_NE(logger, nullptr);
  ASSERT_TRUE(control.Pause());
  const std::string payload(kMaxLogBytes * 2, 'q');
  int evaluated = 0;
  for (int index = 0; index < 100; ++index) {
    YQ_LOG_INFO(*logger, "{}", std::string_view(payload));
  }
  const auto before = logger->counters();
  for (int index = 0; index < 1000; ++index) {
    YQ_LOG_INFO(*logger, "{}", ++evaluated);
  }
  const auto counts = logger->counters();
  EXPECT_GT(counts.queue_dropped_records_total, 0U);
  EXPECT_EQ(counts.accepted_calls_total, 1100U);
  EXPECT_EQ(evaluated, 0);
  EXPECT_EQ(counts.enqueued_records_total, before.enqueued_records_total);
  EXPECT_EQ(counts.truncated_records_total, before.enqueued_records_total);
  CheckConservation(counts);
  control.Resume();
  const auto result = manager.Shutdown();
  EXPECT_TRUE(result.status.ok());
  EXPECT_EQ(Occurrences(ReadFile(directory.File("test.log")), "\n"),
            counts.enqueued_records_total);
}

TEST(LoggingFileTest, PartialWritesAndEintrPreserveEveryQueuedByte) {
  IoControl control;
  control.max_write = 7;
  control.interrupt_next_write.store(true);
  ScopedHooks hooks(control);
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* logger = CreateLogger(manager, directory);
  ASSERT_NE(logger, nullptr);
  for (int index = 0; index < 50; ++index) {
    YQ_LOG_INFO(*logger, "partial-sequence={}", index);
  }
  const auto result = manager.Shutdown();
  EXPECT_TRUE(result.status.ok());
  EXPECT_EQ(result.counters.enqueued_records_total, 50U);
  const auto text = ReadFile(directory.File("test.log"));
  EXPECT_EQ(Occurrences(text, "\n"), 50U);
  std::size_t position = 0;
  for (int index = 0; index < 50; ++index) {
    const auto token = "partial-sequence=" + std::to_string(index) + "\n";
    position = text.find(token, position);
    ASSERT_NE(position, std::string::npos) << index;
    position += token.size();
  }
  EXPECT_GT(control.write_calls.load(), text.size() / 7);
  EXPECT_GT(control.sync_calls.load(), 0U);
  EXPECT_GT(control.close_calls.load(), 0U);
}

TEST(LoggingFileTest, WriteFailureIsLatchedWhileHealthyChannelDrains) {
  IoControl control;
  ScopedHooks hooks(control);
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* fault = CreateLogger(manager, directory, "fault");
  Logger* healthy = CreateLogger(manager, directory, "healthy");
  ASSERT_NE(fault, nullptr);
  ASSERT_NE(healthy, nullptr);
  EXPECT_EQ(fault->backend_status().last_write_monotonic_ns, 0U);
  EXPECT_EQ(healthy->backend_status().last_write_monotonic_ns, 0U);
  EXPECT_TRUE(fault->backend_status().status.ok());
  YQ_LOG_INFO(*fault, "fault-before-failure");
  YQ_LOG_INFO(*healthy, "healthy-before-failure");
  ASSERT_TRUE(WaitFor([&] {
    return fault->backend_status().last_write_monotonic_ns > 0 &&
           healthy->backend_status().last_write_monotonic_ns > 0;
  }));
  const auto fault_progress = fault->backend_status().last_write_monotonic_ns;
  const auto healthy_progress =
      healthy->backend_status().last_write_monotonic_ns;
  control.fail_write.store(true);
  YQ_LOG_ERROR(*fault, "fault-record");
  ASSERT_TRUE(WaitFor([&] { return !fault->backend_status().status.ok(); }));
  const auto live_error = fault->backend_status();
  EXPECT_EQ(live_error.status.code, ErrorCode::kIoError);
  EXPECT_EQ(live_error.status.system_error.value(), ENOSPC);
  EXPECT_EQ(live_error.stage, LogErrorStage::kWrite);
  EXPECT_EQ(live_error.last_write_monotonic_ns, fault_progress);
  for (int index = 0; index < 10; ++index) {
    YQ_LOG_INFO(*healthy, "healthy-record={}", index);
  }
  ASSERT_TRUE(WaitFor([&] {
    return healthy->backend_status().last_write_monotonic_ns > healthy_progress;
  }));
  EXPECT_TRUE(healthy->backend_status().status.ok());
  EXPECT_EQ(healthy->backend_status().stage, LogErrorStage::kNone);
  const auto first = manager.Shutdown();
  const auto close_calls = control.close_calls.load();
  const auto sync_calls = control.sync_calls.load();
  const auto second = manager.Shutdown();
  EXPECT_FALSE(first.status.ok());
  EXPECT_EQ(first.stage, LogErrorStage::kWrite);
  EXPECT_EQ(first.channel_index, 0U);
  EXPECT_GT(first.error_count, 0U);
  EXPECT_EQ(first.error_count, second.error_count);
  EXPECT_EQ(first.stage, second.stage);
  EXPECT_EQ(first.channel_index, second.channel_index);
  EXPECT_EQ(first.counters.enqueued_records_total,
            second.counters.enqueued_records_total);
  EXPECT_EQ(control.close_calls.load(), close_calls);
  EXPECT_EQ(control.sync_calls.load(), sync_calls);
  const auto failed_channel = manager.channel_error(0);
  EXPECT_FALSE(failed_channel.status.ok());
  EXPECT_EQ(failed_channel.stage, LogErrorStage::kWrite);
  EXPECT_GT(failed_channel.error_count, 0U);
  EXPECT_TRUE(manager.channel_error(1).status.ok());
  EXPECT_EQ(
      Occurrences(ReadFile(directory.File("healthy.log")), "healthy-record="),
      10U);
}

TEST(LoggingFileTest, SyncAndCloseFailuresAreReported) {
  for (const bool sync_failure : {true, false}) {
    SCOPED_TRACE(sync_failure);
    IoControl control;
    control.fail_sync.store(sync_failure);
    control.fail_close.store(!sync_failure);
    ScopedHooks hooks(control);
    LogDirectory directory;
    LogManager manager;
    ASSERT_TRUE(directory.valid());
    ASSERT_TRUE(manager.Initialize({}).ok());
    Logger* logger = CreateLogger(manager, directory, "fault");
    ASSERT_NE(logger, nullptr);
    YQ_LOG_INFO(*logger, "written-before-failure");
    const auto result = manager.Shutdown();
    EXPECT_FALSE(result.status.ok());
    EXPECT_EQ(result.stage,
              sync_failure ? LogErrorStage::kSync : LogErrorStage::kClose);
    EXPECT_GT(result.error_count, 0U);
    EXPECT_NE(
        ReadFile(directory.File("fault.log")).find("written-before-failure"),
        std::string::npos);
  }
}

TEST(LoggingFileTest, RotationPreservesWholeLinesAndDrainsAtShutdown) {
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* logger = CreateLogger(manager, directory, "rotate", LogLevel::kTrace,
                                1U << 20, 256);
  ASSERT_NE(logger, nullptr);
  for (int index = 0; index < 100; ++index) {
    YQ_LOG_INFO(*logger, "rotation-sequence={}", index);
  }
  const auto result = manager.Shutdown();
  EXPECT_TRUE(result.status.ok());
  const auto files = directory.Files();
  ASSERT_GT(files.size(), 1U);
  std::string all_text;
  std::size_t lines = 0;
  for (const auto& file : files) {
    const auto text = ReadFile(file);
    if (!text.empty()) EXPECT_EQ(text.back(), '\n') << file;
    lines += Occurrences(text, "\n");
    all_text += text;
  }
  EXPECT_EQ(lines, 100U);
  for (int index = 0; index < 100; ++index) {
    EXPECT_EQ(Occurrences(all_text,
                          "rotation-sequence=" + std::to_string(index) + "\n"),
              1U);
  }
}

TEST(LoggingLifecycleTest, DestructorDrainsPendingRecords) {
  LogDirectory directory;
  ASSERT_TRUE(directory.valid());
  {
    LogManager manager;
    ASSERT_TRUE(manager.Initialize({}).ok());
    Logger* logger = CreateLogger(manager, directory);
    ASSERT_NE(logger, nullptr);
    for (int index = 0; index < 100; ++index) {
      YQ_LOG_INFO(*logger, "destructor-drain={}", index);
    }
  }
  EXPECT_EQ(
      Occurrences(ReadFile(directory.File("test.log")), "destructor-drain="),
      100U);
}

struct ErrorReport {
  int calls = 0;
  ShutdownResult result;

  static void Report(const ShutdownResult& result, void* opaque) noexcept {
    auto& report = *static_cast<ErrorReport*>(opaque);
    ++report.calls;
    report.result = result;
  }
};

TEST(LoggingLifecycleTest, DestructorReportsFailureThroughIndependentOutlet) {
  IoControl control;
  control.fail_write.store(true);
  ScopedHooks hooks(control);
  LogDirectory directory;
  ASSERT_TRUE(directory.valid());
  ErrorReport report;
  {
    LogManager manager;
    LogConfig config;
    config.error_reporter = ErrorReport::Report;
    config.error_context = &report;
    ASSERT_TRUE(manager.Initialize(config).ok());
    Logger* logger = CreateLogger(manager, directory, "fault");
    ASSERT_NE(logger, nullptr);
    YQ_LOG_INFO(*logger, "destructor-failure");
  }
  EXPECT_EQ(report.calls, 1);
  EXPECT_FALSE(report.result.status.ok());
  EXPECT_GT(report.result.error_count, 0U);
}

struct ThreadLogContext {
  Logger* logger = nullptr;
  int first = 0;
  int count = 100;
  pthread_t formatter_thread{};
  std::uint64_t caller_thread_id = 0;
  bool argument_called_on_caller = true;

  static int Argument(ThreadLogContext& context, int value) noexcept {
    if (!pthread_equal(pthread_self(), context.formatter_thread)) {
      context.argument_called_on_caller = false;
    }
    return value;
  }

  static void* Run(void* opaque) noexcept {
    auto& context = *static_cast<ThreadLogContext*>(opaque);
    context.formatter_thread = pthread_self();
#if defined(__APPLE__)
    (void)pthread_threadid_np(nullptr, &context.caller_thread_id);
#else
    context.caller_thread_id = static_cast<std::uint64_t>(syscall(SYS_gettid));
#endif
    for (int index = 0; index < context.count; ++index) {
      YQ_LOG_INFO(*context.logger, "thread-sequence={}",
                  Argument(context, context.first + index));
    }
    return nullptr;
  }
};

TEST(LoggingThreadTest, ConcurrentLoggersPreservePerLoggerOrder) {
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* first = CreateLogger(manager, directory, "first");
  Logger* second = CreateLogger(manager, directory, "second");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ThreadLogContext contexts[2]{{first}, {second}};
  pthread_t threads[2]{};
  ASSERT_EQ(
      pthread_create(&threads[0], nullptr, ThreadLogContext::Run, &contexts[0]),
      0);
  const int second_status =
      pthread_create(&threads[1], nullptr, ThreadLogContext::Run, &contexts[1]);
  EXPECT_EQ(second_status, 0);
  EXPECT_EQ(pthread_join(threads[0], nullptr), 0);
  if (second_status == 0) EXPECT_EQ(pthread_join(threads[1], nullptr), 0);
  EXPECT_TRUE(contexts[0].argument_called_on_caller);
  EXPECT_TRUE(contexts[1].argument_called_on_caller);
  EXPECT_TRUE(manager.Shutdown().status.ok());
  const std::array names = {"first.log", "second.log"};
  for (std::size_t channel = 0; channel < names.size(); ++channel) {
    const auto name = names[channel];
    const auto text = ReadFile(directory.File(name));
    EXPECT_NE(
        text.find("[tid=" + std::to_string(contexts[channel].caller_thread_id) +
                  "]"),
        std::string::npos);
    std::size_t position = 0;
    for (int index = 0; index < 100; ++index) {
      const auto token = "thread-sequence=" + std::to_string(index) + "\n";
      position = text.find(token, position);
      ASSERT_NE(position, std::string::npos) << name << ":" << index;
      position += token.size();
    }
  }
}

TEST(LoggingThreadTest, LoggerCanBeTransferredAfterJoiningItsPreviousCaller) {
  LogDirectory directory;
  LogManager manager;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(manager.Initialize({}).ok());
  Logger* logger = CreateLogger(manager, directory);
  ASSERT_NE(logger, nullptr);
  ThreadLogContext first{logger, 0, 50};
  ThreadLogContext second{logger, 50, 50};
  for (auto* context : {&first, &second}) {
    pthread_t thread{};
    ASSERT_EQ(pthread_create(&thread, nullptr, ThreadLogContext::Run, context),
              0);
    ASSERT_EQ(pthread_join(thread, nullptr), 0);
  }
  EXPECT_EQ(logger->counters().enqueued_records_total, 100U);
  EXPECT_TRUE(manager.Shutdown().status.ok());
  const auto text = ReadFile(directory.File("test.log"));
  for (const auto* context : {&first, &second}) {
    EXPECT_NE(
        text.find("[tid=" + std::to_string(context->caller_thread_id) + "]"),
        std::string::npos);
  }
  std::size_t position = 0;
  for (int index = 0; index < 100; ++index) {
    const auto token = "thread-sequence=" + std::to_string(index) + "\n";
    position = text.find(token, position);
    ASSERT_NE(position, std::string::npos) << index;
    position += token.size();
  }
}

TEST(LoggingFileTest, CorruptQueueIsRejectedWhileHealthyChannelDrains) {
  IoControl control;
  ScopedHooks hooks(control);
  LogDirectory directory;
  ASSERT_TRUE(directory.valid());
  LogManager manager;
  ASSERT_TRUE(manager.Initialize({}).ok());
  const std::string path = directory.File("corrupt.log");
  LoggerConfig config;
  config.name = "corrupt";
  config.file_path = path;
  config.queue_capacity = 4096;
  const auto created = manager.CreateLogger(config);
  ASSERT_TRUE(created.status.ok());
  ASSERT_NE(created.logger, nullptr);
  Logger* healthy = CreateLogger(manager, directory, "healthy");
  ASSERT_NE(healthy, nullptr);
  ASSERT_EQ(control.first_logger, created.logger);
  ASSERT_FALSE(control.first_queue_memory.empty());
  ASSERT_TRUE(control.Pause());
  auto* block = reinterpret_cast<internal::SpscControlBlock*>(
      control.first_queue_memory.data());
  block->write.position.store(config.queue_capacity + 1,
                              std::memory_order_release);
  int evaluated = 0;
  YQ_LOG_INFO(*created.logger, "corrupt-rejected={}", ++evaluated);
  YQ_LOG_INFO(*healthy, "healthy-after-corrupt");
  EXPECT_EQ(evaluated, 0);
  EXPECT_EQ(created.logger->counters().queue_errors_total, 1U);
  CheckConservation(created.logger->counters());
  control.Resume();
  const auto result = manager.Shutdown();
  EXPECT_FALSE(result.status.ok());
  EXPECT_EQ(result.stage, LogErrorStage::kQueue);
  EXPECT_GT(result.error_count, 0U);
  EXPECT_NE(
      ReadFile(directory.File("healthy.log")).find("healthy-after-corrupt"),
      std::string::npos);
}

}  // namespace
}  // namespace yquant::base
