#include "yquant/base/logging.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <string_view>

#include "base/internal/logging_backend.h"

namespace yquant::base {
namespace {

std::atomic<const internal::LogBackendHooks*> backend_hooks{nullptr};
constexpr std::size_t kPathCapacity = kMaxLogPathBytes;
constexpr std::size_t kFramesPerPoll = 64;

int OpenFile(const internal::LogBackendHooks* hooks, const char* path,
             int flags, mode_t mode) noexcept {
  int result;
  do {
    result = hooks != nullptr && hooks->open != nullptr
                 ? hooks->open(hooks->context, path, flags, mode)
                 : open(path, flags, mode);
  } while (result < 0 && errno == EINTR);
  return result;
}

ssize_t WriteFile(const internal::LogBackendHooks* hooks, int fd,
                  const void* bytes, std::size_t size) noexcept {
  if (hooks != nullptr && hooks->write != nullptr) {
    return hooks->write(hooks->context, fd, bytes, size);
  }
  return write(fd, bytes, size);
}

int SyncFile(const internal::LogBackendHooks* hooks, int fd) noexcept {
  int result;
  do {
    result = hooks != nullptr && hooks->sync != nullptr
                 ? hooks->sync(hooks->context, fd)
                 : fsync(fd);
  } while (result < 0 && errno == EINTR);
  return result;
}

int CloseFile(const internal::LogBackendHooks* hooks, int fd) noexcept {
  // Retrying close after EINTR can close an unrelated, reused descriptor.
  return hooks != nullptr && hooks->close != nullptr
             ? hooks->close(hooks->context, fd)
             : close(fd);
}

int RenameFile(const internal::LogBackendHooks* hooks, const char* source,
               const char* destination) noexcept {
  return hooks != nullptr && hooks->rename != nullptr
             ? hooks->rename(hooks->context, source, destination)
             : rename(source, destination);
}

bool CopyText(std::string_view input, char* output,
              std::size_t capacity) noexcept {
  if (input.empty() || input.size() >= capacity ||
      input.find('\0') != std::string_view::npos) {
    return false;
  }
  std::memcpy(output, input.data(), input.size());
  output[input.size()] = '\0';
  return true;
}

bool ParentDirectory(std::string_view path, char* output) noexcept {
  const auto slash = path.find_last_of('/');
  if (slash == std::string_view::npos) {
    return CopyText(".", output, kPathCapacity);
  }
  if (slash + 1 == path.size()) {
    return false;
  }
  return CopyText(path.substr(0, std::max<std::size_t>(slash, 1)), output,
                  kPathCapacity);
}

Status SystemStatus(ErrorCode code, int error) noexcept {
  return {code, std::error_code(error, std::generic_category())};
}

void CheckPthread(int error) noexcept {
  if (error != 0) {
    // A failed lock/wait/join after successful initialization is an internal
    // lifecycle violation: resource ownership cannot safely be recovered.
    std::abort();
  }
}

void AddCounters(LogCounters& destination, const LogCounters& source) noexcept {
  destination.accepted_calls_total += source.accepted_calls_total;
  destination.enqueued_records_total += source.enqueued_records_total;
  destination.queue_dropped_records_total += source.queue_dropped_records_total;
  destination.input_rejected_records_total +=
      source.input_rejected_records_total;
  destination.reentrant_records_total += source.reentrant_records_total;
  destination.queue_errors_total += source.queue_errors_total;
  destination.truncated_records_total += source.truncated_records_total;
}

void ReportShutdownError(const ShutdownResult& result) noexcept {
  char message[256];
  const int length = std::snprintf(
      message, sizeof(message),
      "yquant logging shutdown failed: stage=%d channel=%zu error=%d "
      "system_error=%d count=%llu\n",
      static_cast<int>(result.stage), result.channel_index,
      static_cast<int>(result.status.code), result.status.system_error.value(),
      static_cast<unsigned long long>(result.error_count));
  if (length <= 0) {
    return;
  }
  const auto size =
      std::min(static_cast<std::size_t>(length), sizeof(message) - 1);
  std::size_t offset = 0;
  while (offset < size) {
    const auto written = write(STDERR_FILENO, message + offset, size - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return;
    }
    offset += static_cast<std::size_t>(written);
  }
}

}  // namespace

namespace internal {

void SetLogBackendHooksForTesting(const LogBackendHooks* hooks) noexcept {
  backend_hooks.store(hooks, std::memory_order_release);
}

struct LogManagerImpl {
  struct Channel {
    Logger* logger = nullptr;
    SpscQueue consumer;
    std::byte* queue_memory = nullptr;
    std::size_t queue_bytes = 0;
    std::uint64_t queue_capacity = 0;
    std::byte* batch = nullptr;
    std::size_t batch_used = 0;
    int file_fd = -1;
    int directory_fd = -1;
    char file_path[kPathCapacity]{};
    std::uint64_t file_bytes = 0;
    std::uint64_t max_file_bytes = 0;
    std::uint64_t archive_sequence = 0;
    bool failed = false;
    Status first_error;
    LogErrorStage first_stage = LogErrorStage::kNone;
    std::uint64_t error_count = 0;
  };

  LogConfig config;
  const LogBackendHooks* hooks = nullptr;
  Channel* channels = nullptr;
  std::size_t channel_count = 0;
  pthread_mutex_t mutex{};
  pthread_cond_t condition{};
  pthread_t thread{};
  bool mutex_initialized = false;
  bool condition_initialized = false;
  bool thread_started = false;
  bool ready = false;
  bool request_pending = false;
  bool request_done = false;
  bool stop_requested = false;
  bool shutdown_complete = false;
  std::size_t pending_index = 0;
  Status request_status;
  ShutdownResult final_result;

  void Lock() noexcept { CheckPthread(pthread_mutex_lock(&mutex)); }
  void Unlock() noexcept { CheckPthread(pthread_mutex_unlock(&mutex)); }
  void Signal() noexcept { CheckPthread(pthread_cond_signal(&condition)); }
  void Wait() noexcept { CheckPthread(pthread_cond_wait(&condition, &mutex)); }

  void RecordError(Channel& channel, std::size_t index, LogErrorStage stage,
                   Status status) noexcept {
    channel.failed = true;
    channel.logger->active_.store(0, std::memory_order_release);
    ++channel.error_count;
    ++final_result.error_count;
    if (channel.first_error.ok()) {
      channel.first_error = status;
      channel.first_stage = stage;
      // Publish the first error as one atomic value for live monitoring.
      const auto packed =
          static_cast<std::uint64_t>(status.code) |
          (static_cast<std::uint64_t>(stage) << 8) |
          (static_cast<std::uint64_t>(
               static_cast<std::uint32_t>(status.system_error.value()))
           << 16);
      channel.logger->backend_error_.store(packed, std::memory_order_release);
    }
    if (final_result.status.ok()) {
      final_result.status = status;
      final_result.stage = stage;
      final_result.channel_index = index;
    }
  }

  void IoError(Channel& channel, std::size_t index,
               LogErrorStage stage) noexcept {
    const int saved_errno = errno;
    RecordError(channel, index, stage,
                SystemStatus(ErrorCode::kIoError, saved_errno));
  }

  Status PrepareChannel(Channel& channel) noexcept {
    const auto bytes = SpscQueue::RequiredBytes(channel.queue_capacity);
    channel.queue_bytes = bytes.bytes;
    void* region = nullptr;
    const int error = posix_memalign(&region, SpscQueue::required_alignment(),
                                     channel.queue_bytes);
    if (error != 0) {
      return SystemStatus(ErrorCode::kResourceExhausted, error);
    }
    channel.queue_memory = static_cast<std::byte*>(region);
    const std::span memory(channel.queue_memory, channel.queue_bytes);
    if (SpscQueue::Initialize(memory, channel.queue_capacity) !=
            SpscStatus::kSuccess ||
        channel.consumer.Attach(memory, SpscQueue::Role::kConsumer) !=
            SpscStatus::kSuccess ||
        channel.logger->producer_.Attach(memory, SpscQueue::Role::kProducer) !=
            SpscStatus::kSuccess) {
      return {ErrorCode::kInternalError, {}};
    }
    channel.batch = static_cast<std::byte*>(std::malloc(config.batch_bytes));
    if (channel.batch == nullptr) {
      return {ErrorCode::kResourceExhausted, {}};
    }
    char directory[kPathCapacity];
    if (!ParentDirectory(channel.file_path, directory)) {
      return {ErrorCode::kInvalidArgument, {}};
    }
    channel.directory_fd =
        OpenFile(hooks, directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (channel.directory_fd < 0) {
      return SystemStatus(ErrorCode::kIoError, errno);
    }
    channel.file_fd =
        OpenFile(hooks, channel.file_path,
                 O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NONBLOCK, 0644);
    if (channel.file_fd < 0) {
      return SystemStatus(ErrorCode::kIoError, errno);
    }
    struct stat metadata{};
    if (fstat(channel.file_fd, &metadata) < 0) {
      return SystemStatus(ErrorCode::kIoError, errno);
    }
    if (!S_ISREG(metadata.st_mode)) {
      return {ErrorCode::kInvalidArgument, {}};
    }
    for (std::size_t index = 0; index < channel_count; ++index) {
      if (channels[index].file_fd < 0) {
        continue;
      }
      struct stat existing{};
      if (fstat(channels[index].file_fd, &existing) < 0) {
        return SystemStatus(ErrorCode::kIoError, errno);
      }
      if (existing.st_dev == metadata.st_dev &&
          existing.st_ino == metadata.st_ino) {
        return {ErrorCode::kInvalidArgument, {}};
      }
    }
    if (metadata.st_size > 0) {
      channel.file_bytes = static_cast<std::uint64_t>(metadata.st_size);
    }
    // Touch the entire queue and batch allocations before publishing a Logger.
    // Queue initialization has already installed its atomic control objects.
    const auto control_bytes = channel.queue_bytes - channel.queue_capacity;
    std::memset(channel.queue_memory + control_bytes, 0,
                channel.queue_capacity);
    std::memset(channel.batch, 0, config.batch_bytes);
    if (hooks != nullptr && hooks->on_queue_ready != nullptr) {
      hooks->on_queue_ready(hooks->context, channel.logger, memory);
    }
    channel.logger->active_.store(1, std::memory_order_release);
    return {};
  }

  // Used before a failed CreateLogger publishes its pointer. No endpoint has
  // escaped to a producer, so both views may be detached here.
  void RollBackChannel(Channel& channel) noexcept {
    channel.logger->active_.store(0, std::memory_order_release);
    if (channel.logger->producer_.attached()) {
      (void)channel.logger->producer_.Detach();
    }
    if (channel.consumer.attached()) {
      (void)channel.consumer.Detach();
    }
    if (channel.file_fd >= 0) {
      (void)CloseFile(hooks, channel.file_fd);
      channel.file_fd = -1;
    }
    if (channel.directory_fd >= 0) {
      (void)CloseFile(hooks, channel.directory_fd);
      channel.directory_fd = -1;
    }
    std::free(channel.batch);
    channel.batch = nullptr;
    std::free(channel.queue_memory);
    channel.queue_memory = nullptr;
    channel.queue_bytes = 0;
    channel.batch_used = 0;
    channel.file_bytes = 0;
  }

  bool Flush(Channel& channel, std::size_t index) noexcept {
    std::size_t offset = 0;
    while (offset < channel.batch_used) {
      const auto result =
          WriteFile(hooks, channel.file_fd, channel.batch + offset,
                    channel.batch_used - offset);
      if (result < 0 && errno == EINTR) {
        continue;
      }
      if (result <= 0 ||
          static_cast<std::size_t>(result) > channel.batch_used - offset) {
        if (result >= 0) {
          errno = EIO;
        }
        IoError(channel, index, LogErrorStage::kWrite);
        if (offset != 0) {
          std::memmove(channel.batch, channel.batch + offset,
                       channel.batch_used - offset);
          channel.batch_used -= offset;
        }
        return false;
      }
      offset += static_cast<std::size_t>(result);
      channel.file_bytes += static_cast<std::uint64_t>(result);
      timespec progress{};
      if (clock_gettime(CLOCK_MONOTONIC, &progress) == 0) {
        channel.logger->last_write_ns_.store(
            static_cast<std::uint64_t>(progress.tv_sec) * 1000000000ULL +
                static_cast<std::uint64_t>(progress.tv_nsec),
            std::memory_order_release);
      }
    }
    channel.batch_used = 0;
    return true;
  }

  bool Rotate(Channel& channel, std::size_t index) noexcept {
    if (SyncFile(hooks, channel.file_fd) < 0) {
      IoError(channel, index, LogErrorStage::kSync);
      return false;
    }
    const int old_fd = channel.file_fd;
    channel.file_fd = -1;
    if (CloseFile(hooks, old_fd) < 0) {
      IoError(channel, index, LogErrorStage::kClose);
      return false;
    }
    char archive[kPathCapacity];
    for (;;) {
      if (channel.archive_sequence ==
          std::numeric_limits<std::uint64_t>::max()) {
        errno = EOVERFLOW;
        IoError(channel, index, LogErrorStage::kRotate);
        return false;
      }
      ++channel.archive_sequence;
      const int length = std::snprintf(
          archive, sizeof(archive), "%s.%llu", channel.file_path,
          static_cast<unsigned long long>(channel.archive_sequence));
      if (length < 0 || static_cast<std::size_t>(length) >= sizeof(archive)) {
        errno = ENAMETOOLONG;
        IoError(channel, index, LogErrorStage::kRotate);
        return false;
      }
      struct stat metadata{};
      if (lstat(archive, &metadata) < 0) {
        if (errno == ENOENT) {
          break;
        }
        IoError(channel, index, LogErrorStage::kRotate);
        return false;
      }
    }
    if (RenameFile(hooks, channel.file_path, archive) < 0) {
      IoError(channel, index, LogErrorStage::kRotate);
      return false;
    }
    if (SyncFile(hooks, channel.directory_fd) < 0) {
      IoError(channel, index, LogErrorStage::kSync);
      return false;
    }
    channel.file_fd =
        OpenFile(hooks, channel.file_path,
                 O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NONBLOCK, 0644);
    if (channel.file_fd < 0) {
      IoError(channel, index, LogErrorStage::kOpen);
      return false;
    }
    channel.file_bytes = 0;
    if (SyncFile(hooks, channel.directory_fd) < 0) {
      IoError(channel, index, LogErrorStage::kSync);
      return false;
    }
    return true;
  }

  // Returns true if the frame budget was exhausted and another poll is needed.
  bool Drain(Channel& channel, std::size_t index) noexcept {
    if (channel.failed) {
      return false;
    }
    if (channel.logger->queue_fault_.load(std::memory_order_acquire) != 0) {
      RecordError(channel, index, LogErrorStage::kQueue,
                  {ErrorCode::kCorrupt, {}});
      return false;
    }
    for (std::size_t count = 0; count < kFramesPerPoll; ++count) {
      const auto previous_bytes = channel.batch_used;
      const auto result = channel.consumer.TryRead(std::span(
          channel.batch + previous_bytes, config.batch_bytes - previous_bytes));
      if (result.status == SpscStatus::kEmpty) {
        (void)Flush(channel, index);
        return false;
      }
      if (result.status == SpscStatus::kBufferTooSmall) {
        if (result.payload_size > kMaxLogBytes || previous_bytes == 0) {
          RecordError(channel, index, LogErrorStage::kQueue,
                      {ErrorCode::kCorrupt, {}});
          return false;
        }
        if (!Flush(channel, index)) {
          return false;
        }
        continue;
      }
      if (result.status != SpscStatus::kSuccess) {
        RecordError(channel, index, LogErrorStage::kQueue,
                    {ErrorCode::kCorrupt, {}});
        return false;
      }
      const auto incoming_bytes = result.payload_size;
      if (incoming_bytes == 0 || incoming_bytes > kMaxLogBytes ||
          channel.batch[previous_bytes + incoming_bytes - 1] !=
              std::byte{'\n'}) {
        RecordError(channel, index, LogErrorStage::kQueue,
                    {ErrorCode::kCorrupt, {}});
        return false;
      }
      const auto limit = channel.max_file_bytes;
      if (limit != 0 && (channel.file_bytes != 0 || previous_bytes != 0) &&
          (channel.file_bytes >= limit ||
           previous_bytes > limit - channel.file_bytes ||
           incoming_bytes > limit - channel.file_bytes - previous_bytes)) {
        if (!Flush(channel, index) || !Rotate(channel, index)) {
          return false;
        }
        // TryRead has already copied the incoming frame after the previous
        // batch. Flush and Rotate do not alter those bytes.
        std::memmove(channel.batch, channel.batch + previous_bytes,
                     incoming_bytes);
        channel.batch_used = incoming_bytes;
      } else {
        channel.batch_used += incoming_bytes;
      }
    }
    return Flush(channel, index);
  }

  void FinishChannel(Channel& channel, std::size_t index) noexcept {
    if (!channel.failed) {
      (void)Flush(channel, index);
    }
    if (channel.file_fd >= 0) {
      if (SyncFile(hooks, channel.file_fd) < 0) {
        IoError(channel, index, LogErrorStage::kSync);
      }
      const int fd = channel.file_fd;
      channel.file_fd = -1;
      if (CloseFile(hooks, fd) < 0) {
        IoError(channel, index, LogErrorStage::kClose);
      }
    }
    if (channel.directory_fd >= 0) {
      if (SyncFile(hooks, channel.directory_fd) < 0) {
        IoError(channel, index, LogErrorStage::kSync);
      }
      const int fd = channel.directory_fd;
      channel.directory_fd = -1;
      if (CloseFile(hooks, fd) < 0) {
        IoError(channel, index, LogErrorStage::kClose);
      }
    }
    if (channel.consumer.attached() &&
        channel.consumer.Detach() != SpscStatus::kSuccess) {
      std::abort();
    }
    std::free(channel.batch);
    channel.batch = nullptr;
    std::free(channel.queue_memory);
    channel.queue_memory = nullptr;
  }

  void WaitIdle() noexcept {
    if (config.idle_sleep_ns == 0) {
      return;
    }
    struct timespec deadline{};
    if (clock_gettime(CLOCK_REALTIME, &deadline) < 0) {
      return;
    }
    const auto nanoseconds =
        static_cast<std::uint64_t>(deadline.tv_nsec) + config.idle_sleep_ns;
    deadline.tv_sec += static_cast<time_t>(nanoseconds / 1000000000);
    deadline.tv_nsec = static_cast<long>(nanoseconds % 1000000000);
    Lock();
    if (!request_pending && !stop_requested) {
      const int error = pthread_cond_timedwait(&condition, &mutex, &deadline);
      if (error != ETIMEDOUT) {
        CheckPthread(error);
      }
    }
    Unlock();
  }

  void Run() noexcept {
    Lock();
    ready = true;
    Signal();
    Unlock();
    for (;;) {
      if (hooks != nullptr && hooks->before_poll != nullptr) {
        hooks->before_poll(hooks->context);
      }
      Lock();
      const bool prepare = request_pending;
      const auto index = pending_index;
      Unlock();
      if (prepare) {
        const auto status = PrepareChannel(channels[index]);
        if (!status.ok()) {
          RollBackChannel(channels[index]);
        }
        Lock();
        request_status = status;
        if (status.ok()) {
          ++channel_count;
        }
        request_pending = false;
        request_done = true;
        Signal();
        Unlock();
      }
      Lock();
      const auto count = channel_count;
      const bool stopping = stop_requested;
      Unlock();
      bool more = false;
      for (std::size_t channel = 0; channel < count; ++channel) {
        more |= Drain(channels[channel], channel);
      }
      if (stopping && !more) {
        break;
      }
      if (!more && !stopping) {
        WaitIdle();
      }
    }
    for (std::size_t index = 0; index < channel_count; ++index) {
      FinishChannel(channels[index], index);
    }
  }

  static void* ThreadMain(void* argument) noexcept {
    static_cast<LogManagerImpl*>(argument)->Run();
    return nullptr;
  }
};

}  // namespace internal

LogManager::~LogManager() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  if (!impl_->shutdown_complete) {
    const auto result = Shutdown();
    if (!result.status.ok()) {
      if (impl_->config.error_reporter != nullptr) {
        impl_->config.error_reporter(result, impl_->config.error_context);
      } else {
        // This outlet is independent of the failed logging subsystem.
        ReportShutdownError(result);
      }
    }
  }
  delete[] impl_->channels;
  if (impl_->condition_initialized) {
    CheckPthread(pthread_cond_destroy(&impl_->condition));
  }
  if (impl_->mutex_initialized) {
    CheckPthread(pthread_mutex_destroy(&impl_->mutex));
  }
  delete impl_;
}

Status LogManager::Initialize(const LogConfig& config) noexcept {
  if (impl_ != nullptr) {
    return {ErrorCode::kInvalidState, {}};
  }
  if (config.max_loggers == 0 || config.batch_bytes < kMaxLogBytes ||
      config.batch_bytes >
          static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()) ||
      config.max_loggers > (std::numeric_limits<std::size_t>::max() -
                            sizeof(internal::LogManagerImpl::Channel)) /
                               sizeof(internal::LogManagerImpl::Channel) ||
      config.idle_sleep_ns >
          std::numeric_limits<std::uint64_t>::max() - 1000000000) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  auto* candidate = new (std::nothrow) internal::LogManagerImpl;
  if (candidate == nullptr) {
    return {ErrorCode::kResourceExhausted, {}};
  }
  candidate->config = config;
  candidate->hooks = backend_hooks.load(std::memory_order_acquire);
  candidate->channels =
      new (std::nothrow) internal::LogManagerImpl::Channel[config.max_loggers];
  if (candidate->channels == nullptr) {
    delete candidate;
    return {ErrorCode::kResourceExhausted, {}};
  }
  int error = pthread_mutex_init(&candidate->mutex, nullptr);
  if (error == 0) {
    candidate->mutex_initialized = true;
    error = pthread_cond_init(&candidate->condition, nullptr);
  }
  if (error == 0) {
    candidate->condition_initialized = true;
    error = pthread_create(&candidate->thread, nullptr,
                           internal::LogManagerImpl::ThreadMain, candidate);
  }
  if (error != 0) {
    if (candidate->condition_initialized) {
      CheckPthread(pthread_cond_destroy(&candidate->condition));
    }
    if (candidate->mutex_initialized) {
      CheckPthread(pthread_mutex_destroy(&candidate->mutex));
    }
    delete[] candidate->channels;
    delete candidate;
    return SystemStatus(ErrorCode::kResourceExhausted, error);
  }
  candidate->thread_started = true;
  candidate->Lock();
  while (!candidate->ready) {
    candidate->Wait();
  }
  candidate->Unlock();
  impl_ = candidate;
  return {};
}

CreateLoggerResult LogManager::CreateLogger(
    const LoggerConfig& config) noexcept {
  if (impl_ == nullptr || impl_->shutdown_complete) {
    return {{ErrorCode::kInvalidState, {}}, nullptr};
  }
  const auto required = SpscQueue::RequiredBytes(config.queue_capacity);
  if (required.status != SpscStatus::kSuccess) {
    return {{ErrorCode::kInvalidArgument, {}}, nullptr};
  }
  if (config.name.empty() || config.name.size() > kMaxLogNameBytes ||
      config.name.find('\0') != std::string_view::npos ||
      config.name.find_first_of("\r\n") != std::string_view::npos ||
      config.file_path.empty() ||
      config.file_path.size() >= kPathCapacity - 21 ||
      config.file_path.find('\0') != std::string_view::npos ||
      config.file_path.back() == '/' ||
      config.queue_capacity < kMaxLogBytes + 4 ||
      config.minimum_level < LogLevel::kTrace ||
      config.minimum_level > LogLevel::kOff) {
    return {{ErrorCode::kInvalidArgument, {}}, nullptr};
  }
  if (impl_->channel_count == impl_->config.max_loggers) {
    return {{ErrorCode::kResourceExhausted, {}}, nullptr};
  }
  for (std::size_t index = 0; index < impl_->channel_count; ++index) {
    if (config.file_path == impl_->channels[index].file_path) {
      return {{ErrorCode::kInvalidArgument, {}}, nullptr};
    }
  }
  auto* logger = new (std::nothrow)
      Logger(config.name, config.minimum_level, config.min_interval_ns);
  if (logger == nullptr) {
    return {{ErrorCode::kResourceExhausted, {}}, nullptr};
  }
  auto& channel = impl_->channels[impl_->channel_count];
  channel.logger = logger;
  (void)CopyText(config.file_path, channel.file_path,
                 sizeof(channel.file_path));
  channel.queue_capacity = config.queue_capacity;
  channel.max_file_bytes = config.max_file_bytes;
  impl_->Lock();
  impl_->pending_index = impl_->channel_count;
  impl_->request_done = false;
  impl_->request_pending = true;
  impl_->Signal();
  while (!impl_->request_done) {
    impl_->Wait();
  }
  const auto status = impl_->request_status;
  impl_->Unlock();
  if (!status.ok()) {
    delete logger;
    channel.logger = nullptr;
    return {status, nullptr};
  }
  return {{}, logger};
}

ShutdownResult LogManager::Shutdown() noexcept {
  if (impl_ == nullptr) {
    return {};
  }
  if (impl_->shutdown_complete) {
    return impl_->final_result;
  }
  // The caller has already joined all producers. Detach their views before
  // the backend can release the queue storage.
  for (std::size_t index = 0; index < impl_->channel_count; ++index) {
    auto* logger = impl_->channels[index].logger;
    logger->active_.store(0, std::memory_order_release);
    if (logger->producer_.attached() &&
        logger->producer_.Detach() != SpscStatus::kSuccess) {
      std::abort();
    }
  }
  impl_->Lock();
  impl_->stop_requested = true;
  impl_->Signal();
  impl_->Unlock();
  if (impl_->thread_started) {
    CheckPthread(pthread_join(impl_->thread, nullptr));
    impl_->thread_started = false;
  }
  for (std::size_t index = 0; index < impl_->channel_count; ++index) {
    auto* logger = impl_->channels[index].logger;
    AddCounters(impl_->final_result.counters, logger->counters());
    delete logger;
    impl_->channels[index].logger = nullptr;
  }
  impl_->shutdown_complete = true;
  return impl_->final_result;
}

LogChannelError LogManager::channel_error(std::size_t index) const noexcept {
  if (impl_ == nullptr || !impl_->shutdown_complete) {
    return {{ErrorCode::kInvalidState, {}}, LogErrorStage::kNone, 0};
  }
  if (index >= impl_->channel_count) {
    return {{ErrorCode::kInvalidArgument, {}}, LogErrorStage::kNone, 0};
  }
  const auto& channel = impl_->channels[index];
  return {channel.first_error, channel.first_stage, channel.error_count};
}

}  // namespace yquant::base
