#include "marketd/recording.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <system_error>
#include <utility>

#include "yquant/base/format.h"
#include "yquant/md/quote_codec.h"

namespace yquant::marketd {
namespace {

using Header = std::array<std::byte, kRecordingHeaderSize>;
using Record = std::array<std::byte, kRecordingRecordSize>;
constexpr char kMagic[] = "YQREC001";
constexpr uint32_t kVersion = 1;
constexpr uint32_t kSealed = 1;
constexpr size_t kBatchRecords = 256;

// All persistent integers are little endian; no native structs reach disk.
template <typename Integer>
void Store(std::byte* destination, Integer value) noexcept {
  for (size_t i = 0; i < sizeof(Integer); ++i) {
    destination[i] = static_cast<std::byte>(value & 0xff);
    value >>= 8;
  }
}

template <typename Integer>
Integer Load(const std::byte* source) noexcept {
  Integer value = 0;
  for (size_t i = 0; i < sizeof(Integer); ++i) {
    value |= std::to_integer<Integer>(source[i]) << (i * 8);
  }
  return value;
}

// FNV-1a detects accidental corruption; it is not an authenticity mechanism.
uint64_t Checksum(std::span<const std::byte> bytes) noexcept {
  uint64_t value = 14695981039346656037ULL;
  for (const auto byte : bytes) {
    value ^= std::to_integer<uint8_t>(byte);
    value *= 1099511628211ULL;
  }
  return value;
}

Header EncodeHeader(bool sealed, uint64_t count,
                    uint64_t last_offset) noexcept {
  Header header{};
  std::memcpy(header.data(), kMagic, 8);
  Store(header.data() + 8, kVersion);
  Store(header.data() + 12, uint32_t{kRecordingHeaderSize});
  Store(header.data() + 16, uint32_t{kRecordingRecordSize});
  Store(header.data() + 20, uint32_t{md::kQuoteFrameSize});
  Store(header.data() + 24, sealed ? kSealed : 0U);
  Store(header.data() + 32, count);
  Store(header.data() + 40, last_offset);
  Store(header.data() + 56, Checksum(std::span(header).first(56)));
  return header;
}

base::Status IoError(int error) noexcept {
  return {base::ErrorCode::kIoError,
          std::error_code(error == 0 ? EIO : error, std::generic_category())};
}

base::Status ReaderFailure(base::Status status, std::span<char> detail,
                           const char* message) noexcept {
  if (!detail.empty()) {
    ::yquant::base::FormatTo(detail.data(), detail.size(), "{}", message);
  }
  return status;
}

// Exact reads retry interrupted syscalls; EOF before size is corrupt input.
base::Status ReadAll(int fd, std::span<std::byte> bytes) noexcept {
  while (!bytes.empty()) {
    const auto count = ::read(fd, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      return IoError(errno);
    }
    if (count == 0) {
      return {base::ErrorCode::kCorrupt, {}};
    }
    bytes = bytes.subspan(static_cast<size_t>(count));
  }
  return {};
}

// A null hook uses write; zero-byte progress is an I/O failure.
int WriteAll(int fd, std::span<const std::byte> bytes,
             const RecordIoHooks* hooks = nullptr) noexcept {
  while (!bytes.empty()) {
    const auto count =
        hooks != nullptr && hooks->write != nullptr
            ? hooks->write(hooks->context, fd, bytes.data(), bytes.size())
            : ::write(fd, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      return errno == 0 ? EIO : errno;
    }
    if (count == 0 || static_cast<size_t>(count) > bytes.size()) {
      return EIO;
    }
    bytes = bytes.subspan(static_cast<size_t>(count));
  }
  return 0;
}

int WriteHeader(int fd, const Header& header) noexcept {
  size_t written = 0;
  while (written < header.size()) {
    const auto count =
        ::pwrite(fd, header.data() + written, header.size() - written,
                 static_cast<off_t>(written));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      return errno == 0 ? EIO : errno;
    }
    if (count == 0) {
      return EIO;
    }
    written += static_cast<size_t>(count);
  }
  return 0;
}

int SyncFile(int fd, const RecordIoHooks& hooks) noexcept {
  int result;
  do {
    result =
        hooks.sync != nullptr ? hooks.sync(hooks.context, fd) : ::fsync(fd);
  } while (result != 0 && errno == EINTR);
  return result == 0 ? 0 : (errno == 0 ? EIO : errno);
}

}  // namespace

base::Status LoadRecordingFile(const char* path, size_t max_records,
                               ReplayData& output,
                               std::span<char> error_detail) noexcept {
  if (!error_detail.empty()) {
    error_detail.front() = '\0';
  }
  if (path == nullptr || path[0] == '\0' || max_records == 0 ||
      max_records > kMaxReplayRecords) {
    return ReaderFailure({base::ErrorCode::kInvalidArgument, {}}, error_detail,
                         "invalid recording path or record limit");
  }
  // Nonblocking open lets fstat reject FIFOs without waiting for a writer.
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    return ReaderFailure(IoError(errno), error_detail, "cannot open recording");
  }
  ReplayData loaded;
  // One exit path below keeps descriptor ownership local to this loader.
  auto status = [&]() noexcept -> base::Status {
    struct stat metadata{};
    if (::fstat(fd, &metadata) != 0) {
      return ReaderFailure(IoError(errno), error_detail,
                           "cannot stat recording");
    }
    if (!S_ISREG(metadata.st_mode)) {
      return ReaderFailure({base::ErrorCode::kInvalidArgument, {}},
                           error_detail, "recording must be a regular file");
    }
    Header header;
    auto read_status = ReadAll(fd, header);
    if (!read_status.ok()) {
      return ReaderFailure(read_status, error_detail,
                           "recording header is truncated or unreadable");
    }
    if (std::memcmp(header.data(), kMagic, 8) != 0 ||
        Load<uint32_t>(header.data() + 8) != kVersion ||
        Load<uint32_t>(header.data() + 12) != kRecordingHeaderSize ||
        Load<uint32_t>(header.data() + 16) != kRecordingRecordSize ||
        Load<uint32_t>(header.data() + 20) != md::kQuoteFrameSize ||
        Load<uint32_t>(header.data() + 28) != 0 ||
        Load<uint64_t>(header.data() + 48) != 0 ||
        Load<uint64_t>(header.data() + 56) !=
            Checksum(std::span(header).first(56))) {
      return ReaderFailure({base::ErrorCode::kCorrupt, {}}, error_detail,
                           "recording header layout or checksum is invalid");
    }
    if (Load<uint32_t>(header.data() + 24) != kSealed) {
      return ReaderFailure({base::ErrorCode::kCorrupt, {}}, error_detail,
                           "recording was not sealed by a successful Finish");
    }
    const auto count = Load<uint64_t>(header.data() + 32);
    const auto last_offset = Load<uint64_t>(header.data() + 40);
    if (count > max_records) {
      return ReaderFailure({base::ErrorCode::kResourceExhausted, {}},
                           error_detail, "recording exceeds max_records");
    }
    const auto expected_size =
        kRecordingHeaderSize + count * kRecordingRecordSize;
    if (metadata.st_size < 0 ||
        static_cast<uint64_t>(metadata.st_size) != expected_size) {
      return ReaderFailure({base::ErrorCode::kCorrupt, {}}, error_detail,
                           "recording length does not match its record count");
    }
    ReplayData data;
    read_status = data.Reserve(static_cast<size_t>(count));
    if (!read_status.ok()) {
      return ReaderFailure(read_status, error_detail,
                           "cannot allocate recording replay storage");
    }
    uint64_t previous_offset = 0;
    std::array<std::byte, kRecordingRecordSize * kBatchRecords> batch;
    for (uint64_t i = 0; i < count; ++i) {
      if (i % kBatchRecords == 0) {
        const auto batch_count = std::min<uint64_t>(count - i, kBatchRecords);
        read_status = ReadAll(
            fd, std::span(batch).first(batch_count * kRecordingRecordSize));
        if (!read_status.ok()) {
          return ReaderFailure(read_status, error_detail,
                               "recording frame is truncated or unreadable");
        }
      }
      const auto encoded = std::span(batch).subspan(
          (i % kBatchRecords) * kRecordingRecordSize, kRecordingRecordSize);
      const auto offset = Load<uint64_t>(encoded.data());
      if (Load<uint64_t>(encoded.data() + 88) != Checksum(encoded.first(88)) ||
          (i == 0 && offset != 0) || offset < previous_offset) {
        return ReaderFailure({base::ErrorCode::kCorrupt, {}}, error_detail,
                             "recording checksum or arrival order is invalid");
      }
      ReplayRecord record;
      record.offset_ns = offset;
      read_status = md::DecodeQuote(encoded.subspan(8, 80), record.quote);
      if (!read_status.ok()) {
        return ReaderFailure(read_status, error_detail,
                             "recording quote frame is invalid");
      }
      read_status = data.Append(record);
      if (!read_status.ok()) {
        return read_status;
      }
      previous_offset = offset;
    }
    if (last_offset != previous_offset) {
      return ReaderFailure({base::ErrorCode::kCorrupt, {}}, error_detail,
                           "recording duration does not match its final frame");
    }
    loaded = std::move(data);
    return {};
  }();
  const int close_result = ::close(fd);
  if (status.ok() && close_result != 0) {
    status = ReaderFailure(IoError(errno), error_detail,
                           "cannot close recording input");
  }
  if (status.ok()) {
    output = std::move(loaded);
  }
  return status;
}

base::Status RecordPublisher::Fail(base::Status status,
                                   const char* detail) noexcept {
  if (failure_.ok()) {
    failure_ = status;
    ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_), "{}",
                             detail);
  }
  return failure_;
}

base::Status RecordPublisher::CheckWorker() noexcept {
  const int error = worker_error_.load(std::memory_order_acquire);
  if (error != 0) {
    return Fail(IoError(error), "recording backend write failed");
  }
  return failure_;
}

void RecordPublisher::ReleaseStorage() noexcept {
  (void)producer_.Detach();
  (void)consumer_.Detach();
  std::free(queue_memory_);
  queue_memory_ = nullptr;
}

base::Status RecordPublisher::Init(const RecordConfig& config,
                                   const RecordIoHooks* hooks) noexcept {
  if (active_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  failure_ = {};
  error_detail_[0] = '\0';
  const auto path_length = ::strnlen(config.path, sizeof(config.path));
  if (path_length == 0 || path_length == sizeof(config.path) ||
      !std::has_single_bit(config.queue_capacity) ||
      config.queue_capacity < 256 ||
      config.queue_capacity > kMaxRecordingQueueBytes) {
    return Fail({base::ErrorCode::kInvalidArgument, {}},
                "recording requires a path and a power-of-two queue >= 256");
  }
  const auto required = base::SpscQueue::RequiredBytes(config.queue_capacity);
  if (required.status != base::SpscStatus::kSuccess) {
    return Fail({base::ErrorCode::kInvalidArgument, {}},
                "recording queue capacity is invalid");
  }
  const int allocation_error = ::posix_memalign(
      &queue_memory_, base::SpscQueue::required_alignment(), required.bytes);
  if (allocation_error != 0) {
    return Fail({base::ErrorCode::kResourceExhausted,
                 std::error_code(allocation_error, std::generic_category())},
                "cannot allocate recording queue");
  }
  const auto memory =
      std::span(static_cast<std::byte*>(queue_memory_), required.bytes);
  if (base::SpscQueue::Initialize(memory, config.queue_capacity) !=
          base::SpscStatus::kSuccess ||
      producer_.Attach(memory, base::SpscQueue::Role::kProducer) !=
          base::SpscStatus::kSuccess ||
      consumer_.Attach(memory, base::SpscQueue::Role::kConsumer) !=
          base::SpscStatus::kSuccess) {
    ReleaseStorage();
    return Fail({base::ErrorCode::kUnsupported, {}},
                "cannot initialize recording queue");
  }
  char directory[kMaxDataPathBytes + 1];
  const char* slash = std::strrchr(config.path, '/');
  const char* filename = slash == nullptr ? config.path : slash + 1;
  if (slash == nullptr) {
    std::memcpy(directory, ".", 2);
  } else {
    const size_t length =
        slash == config.path ? 1 : static_cast<size_t>(slash - config.path);
    std::memcpy(directory, config.path, length);
    directory[length] = '\0';
  }
  directory_fd_ = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_fd_ < 0) {
    const auto status = IoError(errno);
    ReleaseStorage();
    return Fail(status, "cannot open recording parent directory");
  }
  // The same directory descriptor creates and later syncs the directory entry,
  // even if a directory path is renamed while the recording is running.
  fd_ = ::openat(directory_fd_, filename,
                 O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
  if (fd_ < 0) {
    const auto status = IoError(errno);
    (void)::close(directory_fd_);
    directory_fd_ = -1;
    ReleaseStorage();
    return Fail(status, "cannot create recording exclusively");
  }
  int error = WriteAll(fd_, EncodeHeader(false, 0, 0));
  stopping_.store(false, std::memory_order_relaxed);
  worker_error_.store(0, std::memory_order_relaxed);
  hooks_ = hooks == nullptr ? RecordIoHooks{} : *hooks;
  first_receive_time_ns_ = 0;
  last_offset_ns_ = 0;
  record_count_ = 0;
  if (error == 0) {
    error = ::pthread_create(&thread_, nullptr, ThreadEntry, this);
  }
  if (error != 0) {
    (void)::close(fd_);
    fd_ = -1;
    (void)::unlinkat(directory_fd_, filename, 0);
    (void)::close(directory_fd_);
    directory_fd_ = -1;
    ReleaseStorage();
    return Fail(IoError(error), "cannot initialize recording backend");
  }
  active_ = true;
  return {};
}

base::Status RecordPublisher::Publish(const model::QuoteTick& quote) noexcept {
  if (!active_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  auto status = CheckWorker();
  if (!status.ok()) {
    return status;
  }
  md::QuoteFrame frame;
  status = md::EncodeQuote(quote, frame);
  if (!status.ok()) {
    return Fail(status, "cannot record an invalid quote");
  }
  const auto first =
      record_count_ == 0 ? quote.receive_time_ns : first_receive_time_ns_;
  if (quote.receive_time_ns < first ||
      quote.receive_time_ns - first < last_offset_ns_) {
    return Fail({base::ErrorCode::kInvalidArgument, {}},
                "recording receive_time_ns moved backwards");
  }
  if (record_count_ == std::numeric_limits<uint64_t>::max()) {
    return Fail({base::ErrorCode::kResourceExhausted, {}},
                "recording record count overflow");
  }
  const auto offset = quote.receive_time_ns - first;
  Record record;
  Store(record.data(), offset);
  std::memcpy(record.data() + 8, frame.data(), frame.size());
  Store(record.data() + 88, Checksum(std::span(record).first(88)));
  const auto result = producer_.TryWrite(record);
  if (result != base::SpscStatus::kSuccess) {
    return Fail(
        {result == base::SpscStatus::kFull ? base::ErrorCode::kResourceExhausted
                                           : base::ErrorCode::kInternalError,
         {}},
        result == base::SpscStatus::kFull ? "recording queue is full"
                                          : "recording queue rejected a frame");
  }
  first_receive_time_ns_ = first;
  last_offset_ns_ = offset;
  ++record_count_;
  return {};
}

base::Status RecordPublisher::Service(uint64_t /*now*/) noexcept {
  if (!active_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  return CheckWorker();
}

void* RecordPublisher::ThreadEntry(void* context) noexcept {
  static_cast<RecordPublisher*>(context)->RunWorker();
  return nullptr;
}

void RecordPublisher::RunWorker() noexcept {
  std::array<std::byte, kRecordingRecordSize * kBatchRecords> batch;
  for (;;) {
    // Observe the stop release before checking the queue. A stop observed after
    // an earlier empty result could otherwise miss the final producer commit.
    const bool stopping = stopping_.load(std::memory_order_acquire);
    size_t used = 0;
    bool empty = false;
    while (used < batch.size()) {
      const auto result = consumer_.TryRead(
          std::span(batch).subspan(used, kRecordingRecordSize));
      if (result.status == base::SpscStatus::kEmpty) {
        empty = true;
        break;
      }
      if (result.status != base::SpscStatus::kSuccess ||
          result.payload_size != kRecordingRecordSize) {
        worker_error_.store(EIO, std::memory_order_release);
        return;
      }
      used += kRecordingRecordSize;
    }
    if (used != 0) {
      const int error = WriteAll(fd_, std::span(batch).first(used), &hooks_);
      if (error != 0) {
        worker_error_.store(error, std::memory_order_release);
        return;
      }
    }
    if (empty) {
      if (stopping) {
        return;
      }
      // The producer never wakes the backend through a lock or syscall.
      const timespec delay{0, 200000};
      (void)::nanosleep(&delay, nullptr);
    }
  }
}

base::Status RecordPublisher::Finish() noexcept {
  if (!active_) {
    return failure_;
  }
  stopping_.store(true, std::memory_order_release);
  const int join_error = ::pthread_join(thread_, nullptr);
  if (join_error != 0) {
    // A join failure cannot safely release storage still owned by the worker.
    return Fail(IoError(join_error), "cannot join recording backend");
  }
  active_ = false;
  (void)CheckWorker();
  if (failure_.ok()) {
    int error = SyncFile(fd_, hooks_);
    if (error == 0) {
      error =
          WriteHeader(fd_, EncodeHeader(true, record_count_, last_offset_ns_));
    }
    if (error == 0) {
      error = SyncFile(fd_, hooks_);
    }
    if (error != 0) {
      (void)Fail(IoError(error), "cannot seal or sync recording");
    }
  }
  const int closed = hooks_.close != nullptr ? hooks_.close(hooks_.context, fd_)
                                             : ::close(fd_);
  if (closed != 0) {
    (void)Fail(IoError(errno), "cannot close recording output");
  }
  fd_ = -1;
  if (failure_.ok()) {
    // Persist the new directory entry only after all file operations succeed.
    const int error = SyncFile(directory_fd_, {});
    if (error != 0) {
      (void)Fail(IoError(error), "cannot sync recording parent directory");
    }
  }
  if (::close(directory_fd_) != 0) {
    (void)Fail(IoError(errno), "cannot close recording parent directory");
  }
  directory_fd_ = -1;
  ReleaseStorage();
  return failure_;
}

void RecordPublisher::Close() noexcept { (void)Finish(); }

}  // namespace yquant::marketd
