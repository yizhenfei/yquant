#include "tradingd/journal.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <system_error>

#include "yquant/base/file.h"
#include "yquant/base/format.h"
#include "yquant/base/path.h"

namespace yquant::tradingd {
namespace {

constexpr size_t kSegmentHeaderBytes = 64;
constexpr size_t kRecordHeaderBytes = 40;
constexpr size_t kPublicationBytes = sizeof(uint64_t);
constexpr size_t kRecordAlignment = alignof(uint64_t);
constexpr size_t kMaxRecordBytes = kRecordHeaderBytes +
                                   kJournalMaxPayloadBytes + kPublicationBytes +
                                   kRecordAlignment - 1;
constexpr size_t kSegmentEndPayloadBytes = 16;
constexpr size_t kSegmentEndBytes =
    kRecordHeaderBytes + kSegmentEndPayloadBytes + kPublicationBytes;
constexpr size_t kFileHeaderBytes = 64;
constexpr size_t kMaxFileNameBytes = 96;
constexpr uint32_t kStorageVersion = 1;
constexpr uint64_t kPublicationMarker = 0x31544d4f43515759ULL;
constexpr uint64_t kNanosecondsPerMillisecond = 1000000;
constexpr uint64_t kNanosecondsPerSecond = 1000000000;
constexpr char kSegmentMagic[] = "YQWAL001";
constexpr char kFileMagic[] = "YQCHK001";
static_assert(std::endian::native == std::endian::little);

template <typename Integer>
void Store(std::byte* target, Integer value) noexcept {
  for (size_t i = 0; i < sizeof(Integer); ++i) {
    target[i] = static_cast<std::byte>(value & 0xff);
    value >>= 8;
  }
}

template <typename Integer>
Integer Load(const std::byte* source) noexcept {
  Integer value = 0;
  for (size_t i = 0; i < sizeof(Integer); ++i) {
    value |= static_cast<Integer>(std::to_integer<unsigned char>(source[i]))
             << (i * 8);
  }
  return value;
}

constexpr auto MakeCrcTable() noexcept {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < table.size(); ++i) {
    auto value = i;
    for (unsigned bit = 0; bit < 8; ++bit) {
      value = (value >> 1) ^ ((value & 1) ? 0x82f63b78U : 0U);
    }
    table[i] = value;
  }
  return table;
}
constexpr auto kCrcTable = MakeCrcTable();

// Reflected Castagnoli CRC32C, with the customary initial/final complement.
uint32_t ExtendCrc(uint32_t crc, std::span<const std::byte> bytes) noexcept {
  for (const auto byte : bytes) {
    crc = kCrcTable[(crc ^ std::to_integer<unsigned char>(byte)) & 0xff] ^
          (crc >> 8);
  }
  return crc;
}

uint32_t Crc(std::span<const std::byte> bytes) noexcept {
  return ~ExtendCrc(~uint32_t{0}, bytes);
}

uint32_t RecordCrc(const std::byte* record, size_t payload_bytes) noexcept {
  auto crc = ExtendCrc(~uint32_t{0}, {record, 32});
  crc = ExtendCrc(crc, {record + 36, kRecordHeaderBytes - 36});
  return ~ExtendCrc(crc, {record + kRecordHeaderBytes, payload_bytes});
}

base::Status IoError(int error = errno) noexcept {
  return {base::ErrorCode::kIoError,
          std::error_code(error == 0 ? EIO : error, std::generic_category())};
}

size_t RecordBytes(size_t payload_bytes) noexcept {
  return (kRecordHeaderBytes + payload_bytes + kPublicationBytes +
          kRecordAlignment - 1) &
         ~(kRecordAlignment - 1);
}

// The final naturally aligned marker is published only after the complete body.
// Storage ordering still requires CRC validation after a system failure.
void WriteRecord(std::byte* target, uint16_t type, uint64_t stream_id,
                 uint64_t lsn, std::span<const std::byte> payload) noexcept {
  const auto bytes = RecordBytes(payload.size());
  std::memset(target, 0, bytes);
  Store(target, static_cast<uint32_t>(bytes));
  Store(target + 4, static_cast<uint32_t>(payload.size()));
  Store(target + 8, type);
  Store(target + 10, static_cast<uint16_t>(kStorageVersion));
  Store(target + 16, stream_id);
  Store(target + 24, lsn);
  if (!payload.empty()) {
    std::memcpy(target + kRecordHeaderBytes, payload.data(), payload.size());
  }
  Store(target + 32, RecordCrc(target, payload.size()));
  auto* marker =
      reinterpret_cast<uint64_t*>(target + bytes - kPublicationBytes);
  __atomic_store_n(marker, kPublicationMarker, __ATOMIC_RELEASE);
}

int SyncFile(int fd) noexcept {
  while (::fsync(fd) != 0) {
    if (errno != EINTR) {
      return errno;
    }
  }
#if defined(__APPLE__)
  while (::fcntl(fd, F_FULLFSYNC) != 0) {
    if (errno != EINTR) {
      return errno;
    }
  }
#endif
  return 0;
}

int SyncDirectory(int fd) noexcept {
  while (::fsync(fd) != 0) {
    if (errno != EINTR) {
      return errno;
    }
  }
  return 0;
}

int SyncMapping(int fd, void* mapping, size_t bytes) noexcept {
  while (::msync(mapping, bytes, MS_SYNC) != 0) {
    if (errno != EINTR) {
      return errno;
    }
  }
  return SyncFile(fd);
}

int WriteAll(int fd, std::span<const std::byte> bytes) noexcept {
  while (!bytes.empty()) {
    const auto written = ::write(fd, bytes.data(), bytes.size());
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return written < 0 ? errno : EIO;
    }
    bytes = bytes.subspan(static_cast<size_t>(written));
  }
  return 0;
}

int ReadAll(int fd, std::span<std::byte> bytes) noexcept {
  while (!bytes.empty()) {
    const auto count = ::read(fd, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return count < 0 ? errno : EIO;
    }
    bytes = bytes.subspan(static_cast<size_t>(count));
  }
  return 0;
}

// Copies from a stable file prefix without sharing the descriptor's offset.
// A premature EOF is an I/O failure, including a concurrent truncation.
int PreadAll(int fd, uint64_t offset, std::span<std::byte> bytes) noexcept {
  while (!bytes.empty()) {
    const auto count =
        ::pread(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return count < 0 ? errno : EIO;
    }
    offset += static_cast<size_t>(count);
    bytes = bytes.subspan(static_cast<size_t>(count));
  }
  return 0;
}

bool ValidName(const char* name) noexcept {
  if (name == nullptr || *name == '\0') {
    return false;
  }
  const auto size = ::strnlen(name, kMaxFileNameBytes + 1);
  if (size > kMaxFileNameBytes || std::strcmp(name, ".") == 0 ||
      std::strcmp(name, "..") == 0) {
    return false;
  }
  for (size_t i = 0; i < size; ++i) {
    const char ch = name[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_')) {
      return false;
    }
  }
  // Control writes must never replace the inode carrying the directory lock.
  return std::strcmp(name, "LOCK") != 0;
}

void SegmentName(char (&name)[48], uint64_t sequence) noexcept {
  ::yquant::base::FormatTo(name, sizeof(name), "segment-{:020}.wal", sequence);
}

void StreamName(char (&name)[48], uint64_t stream_id) noexcept {
  ::yquant::base::FormatTo(name, sizeof(name), "stream-{:020}", stream_id);
}

// Validates the immutable header and exact preallocated file size. Failure
// preserves segment_bytes; no record may be inspected before this succeeds.
base::Status ReadSegmentHeader(int fd, uint64_t stream_id, uint64_t sequence,
                               uint64_t& segment_bytes) noexcept {
  struct stat metadata{};
  std::array<std::byte, kSegmentHeaderBytes> header{};
  if (::fstat(fd, &metadata) != 0) {
    return IoError();
  }
  if (metadata.st_size < static_cast<off_t>(header.size())) {
    return {base::ErrorCode::kCorrupt, {}};
  }
  if (const auto error = PreadAll(fd, 0, header); error != 0) {
    return IoError(error);
  }
  const auto bytes = Load<uint64_t>(header.data() + 32);
  if (std::memcmp(header.data(), kSegmentMagic, 8) != 0 ||
      Load<uint32_t>(header.data() + 8) != kStorageVersion ||
      Load<uint32_t>(header.data() + 12) != kSegmentHeaderBytes ||
      Load<uint64_t>(header.data() + 16) != stream_id ||
      Load<uint64_t>(header.data() + 24) != sequence ||
      Load<uint32_t>(header.data() + 40) != Crc(std::span(header).first(40)) ||
      bytes < kJournalMinSegmentBytes || bytes > kJournalMaxSegmentBytes ||
      (bytes & (bytes - 1)) != 0 ||
      bytes != static_cast<uint64_t>(metadata.st_size)) {
    return {base::ErrorCode::kCorrupt, {}};
  }
  segment_bytes = bytes;
  return {};
}

// Checks all framing and content fields before any callback sees the copy.
bool ValidRecord(std::span<const std::byte> record,
                 uint64_t stream_id) noexcept {
  const auto* bytes = record.data();
  const auto payload_size = Load<uint32_t>(bytes + 4);
  return payload_size <= kJournalMaxPayloadBytes &&
         Load<uint32_t>(bytes) == record.size() &&
         record.size() == RecordBytes(payload_size) &&
         Load<uint16_t>(bytes + 10) == kStorageVersion &&
         Load<uint64_t>(bytes + 16) == stream_id &&
         Load<uint64_t>(bytes + record.size() - kPublicationBytes) ==
             kPublicationMarker &&
         Load<uint32_t>(bytes + 32) == RecordCrc(bytes, payload_size);
}

// Checks an untouched suffix in bounded chunks. Only offline recovery uses
// this operation; live scans stop at an explicit stable LSN bound.
base::Status ZeroTail(int fd, uint64_t offset, uint64_t segment_bytes,
                      std::span<std::byte> storage, bool& zero) noexcept {
  zero = true;
  while (offset < segment_bytes) {
    auto part = storage.first(static_cast<size_t>(
        std::min<uint64_t>(storage.size(), segment_bytes - offset)));
    if (const auto error = PreadAll(fd, offset, part); error != 0) {
      return IoError(error);
    }
    if (std::any_of(part.begin(), part.end(),
                    [](std::byte byte) { return byte != std::byte{0}; })) {
      zero = false;
      return {};
    }
    offset += part.size();
  }
  return {};
}

// Common cold-path decoder. Bounded scans require the complete requested
// prefix, while recovery accepts its first damaged/unpublished tail. Only a
// successful call publishes either output; callbacks may already have run.
base::Status ScanStream(int root, const JournalCursor& cursor, bool bounded,
                        uint64_t through_lsn, bool synchronize,
                        JournalReplayCallback callback, void* context,
                        JournalCursor& end,
                        JournalReplayResult& result) noexcept {
  char stream_name[48];
  StreamName(stream_name, cursor.stream_id);
  const int directory =
      ::openat(root, stream_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0) {
    return IoError();
  }
  JournalCursor position = cursor;
  JournalReplayResult recovered{cursor.lsn, 0, false};
  std::array<std::byte, kMaxRecordBytes> storage{};
  base::Status status;
  bool continuation = true;
  while (continuation && status.ok() &&
         (!bounded || position.lsn < through_lsn)) {
    continuation = false;
    char name[48];
    SegmentName(name, position.segment);
    const int fd = ::openat(directory, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      if (errno == ENOENT && position.segment != cursor.segment) {
        if (bounded) {
          status = {base::ErrorCode::kCorrupt, {}};
        } else {
          recovered.tail_discarded = true;
        }
      } else {
        status = IoError();
      }
      break;
    }
    uint64_t segment_bytes = 0;
    status = ReadSegmentHeader(fd, cursor.stream_id, position.segment,
                               segment_bytes);
    const bool valid_header = status.ok();
    if (status.ok() && position.offset > segment_bytes - kSegmentEndBytes) {
      status = {base::ErrorCode::kCorrupt, {}};
    }
    while (status.ok() &&
           position.offset + kRecordHeaderBytes + kPublicationBytes <=
               segment_bytes &&
           (!bounded || position.lsn < through_lsn)) {
      if (const auto error =
              PreadAll(fd, position.offset,
                       std::span(storage).first(kRecordHeaderBytes));
          error != 0) {
        status = IoError(error);
        break;
      }
      const auto length = Load<uint32_t>(storage.data());
      if (length == 0) {
        if (bounded) {
          status = {base::ErrorCode::kCorrupt, {}};
        } else {
          bool zero = false;
          status = ZeroTail(fd, position.offset, segment_bytes, storage, zero);
          recovered.tail_discarded = !zero;
        }
        break;
      }
      const auto payload_size = Load<uint32_t>(storage.data() + 4);
      bool valid = payload_size <= kJournalMaxPayloadBytes &&
                   length == RecordBytes(payload_size) &&
                   length <= segment_bytes - position.offset;
      if (valid) {
        auto record = std::span(storage).first(length);
        if (const auto error = PreadAll(fd, position.offset, record);
            error != 0) {
          status = IoError(error);
          break;
        }
        valid = ValidRecord(record, cursor.stream_id);
      }
      const auto type = Load<uint16_t>(storage.data() + 8);
      const auto lsn = Load<uint64_t>(storage.data() + 24);
      if (valid && type == 0) {
        valid = payload_size == kSegmentEndPayloadBytes &&
                position.segment != std::numeric_limits<uint64_t>::max() &&
                lsn == position.lsn &&
                Load<uint64_t>(storage.data() + kRecordHeaderBytes) ==
                    position.segment + 1 &&
                Load<uint64_t>(storage.data() + kRecordHeaderBytes + 8) == lsn;
        if (valid) {
          ++position.segment;
          position.offset = kSegmentHeaderBytes;
          continuation = true;
          break;
        }
      } else if (valid) {
        valid = position.lsn != std::numeric_limits<uint64_t>::max() &&
                lsn != 0 && lsn == position.lsn + 1;
      }
      if (!valid) {
        if (bounded) {
          status = {base::ErrorCode::kCorrupt, {}};
        } else {
          recovered.tail_discarded = true;
        }
        break;
      }
      status = callback(context, type, lsn,
                        {storage.data() + kRecordHeaderBytes, payload_size});
      if (!status.ok()) {
        break;
      }
      position.offset += length;
      position.lsn = recovered.last_lsn = lsn;
      ++recovered.records;
    }
    if (synchronize && valid_header) {
      if (const auto error = SyncFile(fd); error != 0 && status.ok()) {
        status = IoError(error);
      }
    }
    if (::close(fd) != 0 && status.ok()) {
      status = IoError();
    }
  }
  if (bounded && status.ok() && position.lsn != through_lsn) {
    status = {base::ErrorCode::kCorrupt, {}};
  }
  if (synchronize) {
    if (const auto error = SyncDirectory(directory);
        error != 0 && status.ok()) {
      status = IoError(error);
    }
    if (const auto error = SyncDirectory(root); error != 0 && status.ok()) {
      status = IoError(error);
    }
  }
  if (::close(directory) != 0 && status.ok()) {
    status = IoError();
  }
  if (status.ok()) {
    end = position;
    result = recovered;
  }
  return status;
}

// Reads one immutable inode, including when its directory entry is replaced.
// Failure preserves size; the supplied storage may have been overwritten.
base::Status LoadFileAt(int directory, const char* name,
                        std::span<std::byte> storage, size_t& size) noexcept {
  if (!ValidName(name)) {
    return {base::ErrorCode::kInvalidArgument, {}};
  }
  const int fd = ::openat(directory, name, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return IoError();
  }
  struct stat metadata{};
  std::array<std::byte, kFileHeaderBytes> header{};
  base::Status status;
  if (::fstat(fd, &metadata) != 0) {
    status = IoError();
  } else if (metadata.st_size < static_cast<off_t>(header.size())) {
    status = {base::ErrorCode::kCorrupt, {}};
  } else if (const auto error = ReadAll(fd, header); error != 0) {
    status = IoError(error);
  } else if (std::memcmp(header.data(), kFileMagic, 8) != 0 ||
             Load<uint32_t>(header.data() + 8) != kStorageVersion ||
             Load<uint32_t>(header.data() + 12) != kFileHeaderBytes ||
             Load<uint32_t>(header.data() + 28) !=
                 Crc(std::span(header).first(28))) {
    status = {base::ErrorCode::kCorrupt, {}};
  }
  const auto payload_size = Load<uint64_t>(header.data() + 16);
  if (status.ok() && (payload_size > kJournalMaxSnapshotBytes ||
                      payload_size + header.size() !=
                          static_cast<uint64_t>(metadata.st_size))) {
    status = {base::ErrorCode::kCorrupt, {}};
  }
  if (status.ok() && payload_size > storage.size()) {
    status = {base::ErrorCode::kResourceExhausted, {}};
  }
  if (status.ok()) {
    auto output = storage.first(static_cast<size_t>(payload_size));
    if (const auto error = ReadAll(fd, output); error != 0) {
      status = IoError(error);
    } else if (Crc(output) != Load<uint32_t>(header.data() + 24)) {
      status = {base::ErrorCode::kCorrupt, {}};
    }
  }
  if (::close(fd) != 0 && status.ok()) {
    status = IoError();
  }
  if (status.ok()) {
    size = static_cast<size_t>(payload_size);
  }
  return status;
}

uint64_t NowNs() noexcept {
  timespec now{};
  ::clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * kNanosecondsPerSecond +
         static_cast<uint64_t>(now.tv_nsec);
}

void PauseWorker() noexcept {
  timespec delay{0, static_cast<long>(kNanosecondsPerMillisecond)};
  while (::nanosleep(&delay, &delay) != 0 && errno == EINTR) {
  }
}

// Newly created nested directory entries need synchronization in their parent
// directories as well as synchronization of files subsequently stored in them.
base::Status SyncParents(const char* directory) noexcept {
  std::string path(directory);
  if (!path.starts_with('/')) {
    std::string current;
    auto status = base::GetWorkingDirectory(current);
    if (!status.ok()) {
      return status;
    }
    path = base::JoinPath(current, path);
  }
  for (;;) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
      return IoError();
    }
    auto sync_error = SyncDirectory(fd);
    if (::close(fd) != 0 && sync_error == 0) {
      sync_error = errno;
    }
    if (sync_error != 0) {
      return IoError(sync_error);
    }
    const auto parent = base::ParentPath(path);
    if (parent == path || parent.empty()) {
      break;
    }
    path = parent;
  }
  return {};
}

}  // namespace

JournalCursor JournalStartCursor(uint64_t stream_id) noexcept {
  return {stream_id, 0, kSegmentHeaderBytes, 0};
}

base::Status ReadJournalFile(const char* directory, const char* name,
                             std::span<std::byte> storage,
                             size_t& size) noexcept {
  if (directory == nullptr || *directory == '\0' || !ValidName(name)) {
    return {base::ErrorCode::kInvalidArgument, {}};
  }
  const int fd = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return IoError();
  }
  size_t loaded_size = 0;
  auto status = LoadFileAt(fd, name, storage, loaded_size);
  if (::close(fd) != 0 && status.ok()) {
    status = IoError();
  }
  if (status.ok()) {
    size = loaded_size;
  }
  return status;
}

base::Status ScanJournal(const char* directory, JournalCursor& cursor,
                         uint64_t through_lsn, JournalReplayCallback callback,
                         void* context) noexcept {
  if (directory == nullptr || *directory == '\0' || cursor.stream_id == 0 ||
      callback == nullptr || through_lsn < cursor.lsn) {
    return {base::ErrorCode::kInvalidArgument, {}};
  }
  if (cursor.offset < kSegmentHeaderBytes ||
      cursor.offset % kRecordAlignment != 0) {
    return {base::ErrorCode::kCorrupt, {}};
  }
  const int fd = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return IoError();
  }
  JournalCursor end;
  JournalReplayResult result;
  auto status = ScanStream(fd, cursor, true, through_lsn, false, callback,
                           context, end, result);
  if (::close(fd) != 0 && status.ok()) {
    status = IoError();
  }
  if (status.ok()) {
    cursor = end;
  }
  return status;
}

base::Status ProbeJournal(const char* directory, uint64_t stream_id,
                          JournalReplayResult& result) noexcept {
  if (directory == nullptr || *directory == '\0' || stream_id == 0) {
    return {base::ErrorCode::kInvalidArgument, {}};
  }
  const int fd = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return IoError();
  }
  JournalCursor end;
  JournalReplayResult recovered;
  const auto ignore = [](void*, uint16_t, uint64_t,
                         std::span<const std::byte>) noexcept -> base::Status {
    return {};
  };
  auto status = ScanStream(fd, JournalStartCursor(stream_id), false, 0, true,
                           ignore, nullptr, end, recovered);
  if (::close(fd) != 0 && status.ok()) {
    status = IoError();
  }
  if (status.ok()) {
    result = recovered;
  }
  return status;
}

Journal::~Journal() noexcept { (void)Close(); }

base::Status Journal::Fail(base::Status status, const char* detail) noexcept {
  if (failure_.ok()) {
    failure_ = status;
    ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_), "{}",
                             detail);
    failed_.store(true, std::memory_order_release);
  }
  return failure_;
}

base::Status Journal::WorkerFailure() noexcept {
  const auto error = worker_error_.load(std::memory_order_acquire);
  return error == 0
             ? failure_
             : Fail(IoError(error), "journal background storage failure");
}

bool Journal::healthy() const noexcept {
  return acquired_.load(std::memory_order_acquire) &&
         !failed_.load(std::memory_order_acquire) &&
         worker_error_.load(std::memory_order_acquire) == 0;
}

const char* Journal::error_detail() const noexcept {
  return worker_error_.load(std::memory_order_acquire) != 0 &&
                 error_detail_[0] == '\0'
             ? "journal background storage failure"
             : error_detail_;
}

base::Status Journal::Acquire(const JournalOptions& options,
                              const JournalIoHooks* hooks) noexcept {
  if (directory_fd_ >= 0) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  failure_ = {};
  failed_.store(false, std::memory_order_relaxed);
  error_detail_[0] = '\0';
  worker_error_.store(0, std::memory_order_relaxed);
  if (options.directory == nullptr || *options.directory == '\0' ||
      options.segment_bytes < kJournalMinSegmentBytes ||
      options.segment_bytes > kJournalMaxSegmentBytes ||
      (options.segment_bytes & (options.segment_bytes - 1)) != 0 ||
      options.reserve_segments < kJournalMinReserveSegments ||
      options.reserve_segments > kJournalMaxReserveSegments ||
      options.sync_interval_ms < kJournalMinSyncIntervalMs ||
      options.sync_interval_ms > kJournalMaxSyncIntervalMs ||
      options.sync_after_bytes < kJournalMinSyncAfterBytes ||
      options.sync_after_bytes > options.segment_bytes) {
    return Fail({base::ErrorCode::kInvalidArgument, {}},
                "invalid journal options");
  }
  bool created = false;
  const auto directory_status =
      base::CreateDirectories(options.directory, created);
  if (!directory_status.ok()) {
    return Fail(directory_status, "cannot create journal directory");
  }
  if (created) {
    const auto status = SyncParents(options.directory);
    if (!status.ok()) {
      return Fail(status, "cannot sync journal directory ancestry");
    }
  }
  directory_fd_ = ::open(options.directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_fd_ < 0) {
    return Fail(IoError(), "cannot open journal directory");
  }
  lock_fd_ =
      ::openat(directory_fd_, "LOCK", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock_fd_ < 0 || ::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    const auto status = IoError();
    if (lock_fd_ >= 0) {
      ::close(lock_fd_);
    }
    ::close(directory_fd_);
    lock_fd_ = directory_fd_ = -1;
    return Fail(status, "cannot acquire journal directory lock");
  }
  options_ = options;
  options_.directory = nullptr;  // The open directory owns the resolved path.
  hooks_ = hooks == nullptr ? JournalIoHooks{} : *hooks;
  published_lsn_.store(0, std::memory_order_relaxed);
  durable_lsn_.store(0, std::memory_order_relaxed);
  acquired_.store(true, std::memory_order_release);
  return {};
}

int Journal::PrepareSegment(uint64_t sequence) noexcept {
  auto& segment = segments_[sequence % kSegmentSlots];
  if (segment.state.load(std::memory_order_acquire) != SegmentState::kFree) {
    return EBUSY;
  }
  char name[48];
  SegmentName(name, sequence);
  const int fd =
      ::openat(stream_fd_, name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    return errno;
  }
  int error = 0;
  if (::ftruncate(fd, static_cast<off_t>(options_.segment_bytes)) != 0) {
    error = errno;
  }
#if defined(__APPLE__)
  if (error == 0) {
    fstore_t allocation{};
    allocation.fst_flags = F_ALLOCATEALL;
    allocation.fst_posmode = F_PEOFPOSMODE;
    allocation.fst_length = static_cast<off_t>(options_.segment_bytes);
    if (::fcntl(fd, F_PREALLOCATE, &allocation) != 0) {
      error = errno;
    }
  }
#else
  if (error == 0) {
    error =
        ::posix_fallocate(fd, 0, static_cast<off_t>(options_.segment_bytes));
  }
#endif
  void* mapping = MAP_FAILED;
  if (error == 0) {
    mapping = ::mmap(nullptr, options_.segment_bytes, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
      error = errno;
    }
  }
  if (error == 0) {
    auto* bytes = static_cast<std::byte*>(mapping);
    const auto page_bytes = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    auto* touches = static_cast<volatile std::byte*>(mapping);
    for (size_t i = 0; i < options_.segment_bytes; i += page_bytes) {
      touches[i] = std::byte{0};
    }
    std::memcpy(bytes, kSegmentMagic, 8);
    Store(bytes + 8, kStorageVersion);
    Store(bytes + 12, static_cast<uint32_t>(kSegmentHeaderBytes));
    Store(bytes + 16, stream_id_);
    Store(bytes + 24, sequence);
    Store(bytes + 32, static_cast<uint64_t>(options_.segment_bytes));
    Store(bytes + 40, Crc({bytes, 40}));
    error = SyncMapping(fd, mapping, page_bytes);
    if (error == 0) {
      error = SyncDirectory(stream_fd_);
    }
  }
  if (error != 0) {
    if (mapping != MAP_FAILED) {
      ::munmap(mapping, options_.segment_bytes);
    }
    ::close(fd);
    // Failed creation never publishes Ready and is not part of the stream.
    (void)::unlinkat(stream_fd_, name, 0);
    return error;
  }
  segment.sequence = sequence;
  segment.fd = fd;
  segment.mapping = static_cast<std::byte*>(mapping);
  segment.published_bytes.store(kSegmentHeaderBytes, std::memory_order_relaxed);
  segment.published_lsn.store(0, std::memory_order_relaxed);
  segment.synced_bytes = kSegmentHeaderBytes;
  segment.synced_lsn = 0;
  segment.state.store(SegmentState::kReady, std::memory_order_release);
  return 0;
}

base::Status Journal::Start(uint64_t stream_id) noexcept {
  if (!healthy() || stream_fd_ >= 0 || stream_id == 0) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  char name[48];
  StreamName(name, stream_id);
  if (::mkdirat(directory_fd_, name, 0700) != 0) {
    return Fail(IoError(), "cannot create fresh journal stream");
  }
  stream_fd_ =
      ::openat(directory_fd_, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (stream_fd_ < 0) {
    return Fail(IoError(), "cannot open fresh journal stream");
  }
  if (const auto error = SyncDirectory(directory_fd_); error != 0) {
    return Fail(IoError(error), "cannot sync journal stream directory");
  }
  stream_id_ = stream_id;
  current_sequence_ = 0;
  current_offset_ = kSegmentHeaderBytes;
  active_sequence_.store(0, std::memory_order_relaxed);
  stopping_.store(false, std::memory_order_relaxed);
  force_flush_.store(false, std::memory_order_relaxed);
  for (size_t i = 0; i <= options_.reserve_segments; ++i) {
    if (const auto error = PrepareSegment(i); error != 0) {
      return Fail(IoError(error), "cannot prepare journal mapping");
    }
  }
  segments_[0].state.store(SegmentState::kActive, std::memory_order_release);
  const auto error = ::pthread_create(&worker_, nullptr, ThreadEntry, this);
  if (error != 0) {
    return Fail(IoError(error), "cannot start journal background worker");
  }
  worker_started_ = true;
  return {};
}

base::Status Journal::TryAppend(uint16_t type,
                                std::span<const std::byte> payload,
                                uint64_t& lsn) noexcept {
  if (!worker_started_ || !healthy()) {
    const auto status = WorkerFailure();
    return status.ok() ? base::Status{base::ErrorCode::kInvalidState, {}}
                       : status;
  }
  if (type == 0 || payload.size() > kJournalMaxPayloadBytes) {
    return {base::ErrorCode::kInvalidArgument, {}};
  }
  const auto previous_lsn = published_lsn_.load(std::memory_order_relaxed);
  if (previous_lsn == std::numeric_limits<uint64_t>::max()) {
    return {base::ErrorCode::kResourceExhausted, {}};
  }
  const auto bytes = RecordBytes(payload.size());
  if (current_offset_ + bytes + kSegmentEndBytes > options_.segment_bytes) {
    const auto next_sequence = current_sequence_ + 1;
    if (next_sequence == 0) {
      return {base::ErrorCode::kResourceExhausted, {}};
    }
    auto& next = segments_[next_sequence % kSegmentSlots];
    if (next.state.load(std::memory_order_acquire) != SegmentState::kReady ||
        next.sequence != next_sequence) {
      return {base::ErrorCode::kResourceExhausted, {}};
    }
    auto& current = segments_[current_sequence_ % kSegmentSlots];
    std::array<std::byte, kSegmentEndPayloadBytes> end{};
    Store(end.data(), next_sequence);
    Store(end.data() + 8, previous_lsn);
    WriteRecord(current.mapping + current_offset_, 0, stream_id_, previous_lsn,
                end);
    current.published_bytes.store(current_offset_ + kSegmentEndBytes,
                                  std::memory_order_release);
    current.state.store(SegmentState::kSealed, std::memory_order_release);
    current_sequence_ = next_sequence;
    current_offset_ = kSegmentHeaderBytes;
    next.state.store(SegmentState::kActive, std::memory_order_release);
    active_sequence_.store(next_sequence, std::memory_order_release);
  }
  auto& current = segments_[current_sequence_ % kSegmentSlots];
  const auto next_lsn = previous_lsn + 1;
  WriteRecord(current.mapping + current_offset_, type, stream_id_, next_lsn,
              payload);
  current_offset_ += bytes;
  current.published_bytes.store(current_offset_, std::memory_order_release);
  current.published_lsn.store(next_lsn, std::memory_order_release);
  published_lsn_.store(next_lsn, std::memory_order_release);
  lsn = next_lsn;
  return {};
}

JournalCursor Journal::GetCursor() const noexcept {
  return {stream_id_, current_sequence_, current_offset_, published_lsn()};
}

int Journal::SyncSegment(Segment& segment, bool sealed) noexcept {
  // Acquire LSN before byte count: the latter may include more complete
  // records, but can never exclude the captured LSN. Acknowledge only that LSN.
  const auto lsn =
      std::min(segment.published_lsn.load(std::memory_order_acquire),
               published_lsn_.load(std::memory_order_acquire));
  const auto bytes = segment.published_bytes.load(std::memory_order_acquire);
  if (bytes != segment.synced_bytes || lsn != segment.synced_lsn || sealed) {
    const auto error =
        hooks_.flush != nullptr
            ? hooks_.flush(hooks_.context, segment.fd, segment.mapping, bytes)
            : SyncMapping(segment.fd, segment.mapping, bytes);
    if (error != 0) {
      return error;
    }
    segment.synced_bytes = bytes;
    segment.synced_lsn = lsn;
  }
  // Sealed segments are visited in order, so no earlier sync can be missing.
  auto durable = durable_lsn_.load(std::memory_order_relaxed);
  if (lsn > durable) {
    durable_lsn_.store(lsn, std::memory_order_release);
  }
  return 0;
}

int Journal::ReleaseSegment(Segment& segment) noexcept {
  int error = 0;
  if (segment.mapping != nullptr &&
      ::munmap(segment.mapping, options_.segment_bytes) != 0) {
    error = errno;
  }
  if (segment.fd >= 0 && ::close(segment.fd) != 0 && error == 0) {
    error = errno;
  }
  segment.mapping = nullptr;
  segment.fd = -1;
  segment.state.store(SegmentState::kFree, std::memory_order_release);
  return error;
}

void* Journal::ThreadEntry(void* context) noexcept {
  static_cast<Journal*>(context)->RunWorker();
  return nullptr;
}

void Journal::RunWorker() noexcept {
  uint64_t flush_sequence = 0;
  uint64_t prepare_sequence = options_.reserve_segments + 1;
  auto last_sync = NowNs();
  for (;;) {
    const bool stopping = stopping_.load(std::memory_order_acquire);
    const bool forced = force_flush_.exchange(false, std::memory_order_acq_rel);
    const auto now = NowNs();
    const bool due = now - last_sync >=
                     options_.sync_interval_ms * kNanosecondsPerMillisecond;
    for (;;) {
      auto& segment = segments_[flush_sequence % kSegmentSlots];
      const auto state = segment.state.load(std::memory_order_acquire);
      if ((state != SegmentState::kActive && state != SegmentState::kSealed) ||
          segment.sequence != flush_sequence) {
        break;
      }
      const bool sealed = state == SegmentState::kSealed;
      const auto bytes =
          segment.published_bytes.load(std::memory_order_acquire);
      if (sealed || stopping || forced || due ||
          bytes - segment.synced_bytes >= options_.sync_after_bytes) {
        if (const auto error = SyncSegment(segment, sealed); error != 0) {
          worker_error_.store(error, std::memory_order_release);
          return;
        }
        last_sync = now;
      }
      if (!sealed) {
        break;
      }
      if (const auto error = ReleaseSegment(segment); error != 0) {
        worker_error_.store(error, std::memory_order_release);
        return;
      }
      ++flush_sequence;
    }
    if (stopping) {
      return;
    }
    const auto target = active_sequence_.load(std::memory_order_acquire) +
                        options_.reserve_segments;
    while (prepare_sequence <= target) {
      const auto error = PrepareSegment(prepare_sequence);
      if (error == EBUSY) {
        break;
      }
      if (error != 0) {
        worker_error_.store(error, std::memory_order_release);
        return;
      }
      ++prepare_sequence;
    }
    PauseWorker();
  }
}

base::Status Journal::Flush() noexcept {
  if (!worker_started_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  const auto target = published_lsn();
  force_flush_.store(true, std::memory_order_release);
  while (durable_lsn() < target &&
         worker_error_.load(std::memory_order_acquire) == 0) {
    PauseWorker();
  }
  return WorkerFailure();
}

base::Status Journal::Close() noexcept {
  acquired_.store(false, std::memory_order_release);
  if (worker_started_) {
    stopping_.store(true, std::memory_order_release);
    const auto error = ::pthread_join(worker_, nullptr);
    if (error != 0) {
      (void)Fail(IoError(error), "cannot join journal background worker");
    }
    worker_started_ = false;
    (void)WorkerFailure();
  }
  for (auto& segment : segments_) {
    if (const auto error = ReleaseSegment(segment); error != 0) {
      (void)Fail(IoError(error), "cannot release journal mapping");
    }
  }
  if (stream_fd_ >= 0 && ::close(stream_fd_) != 0) {
    (void)Fail(IoError(), "cannot close journal stream directory");
  }
  if (lock_fd_ >= 0 && ::close(lock_fd_) != 0) {
    (void)Fail(IoError(), "cannot release journal directory lock");
  }
  if (directory_fd_ >= 0 && ::close(directory_fd_) != 0) {
    (void)Fail(IoError(), "cannot close journal state directory");
  }
  stream_fd_ = lock_fd_ = directory_fd_ = -1;
  return failure_;
}

base::Status Journal::Replay(uint64_t stream_id, JournalReplayCallback callback,
                             void* context,
                             JournalReplayResult& result) noexcept {
  return ReplayFrom(JournalStartCursor(stream_id), callback, context, result);
}

base::Status Journal::ReplayFrom(const JournalCursor& cursor,
                                 JournalReplayCallback callback, void* context,
                                 JournalReplayResult& result) noexcept {
  if (directory_fd_ < 0 || worker_started_ || callback == nullptr ||
      cursor.stream_id == 0) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  if (cursor.offset < kSegmentHeaderBytes ||
      cursor.offset % kRecordAlignment != 0) {
    return {base::ErrorCode::kCorrupt, {}};
  }
  JournalCursor end;
  return ScanStream(directory_fd_, cursor, false, 0, false, callback, context,
                    end, result);
}

base::Status Journal::SaveFile(
    const char* name, std::span<const std::byte> payload) const noexcept {
  if (directory_fd_ < 0) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  if (!ValidName(name) || payload.size() > kJournalMaxSnapshotBytes) {
    return {base::ErrorCode::kInvalidArgument, {}};
  }
  char temporary[kMaxFileNameBytes + 8];
  ::yquant::base::FormatTo(temporary, sizeof(temporary), "{}.tmp", name);
  const int fd = ::openat(directory_fd_, temporary,
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    return IoError();
  }
  std::array<std::byte, kFileHeaderBytes> header{};
  std::memcpy(header.data(), kFileMagic, 8);
  Store(header.data() + 8, kStorageVersion);
  Store(header.data() + 12, static_cast<uint32_t>(kFileHeaderBytes));
  Store(header.data() + 16, static_cast<uint64_t>(payload.size()));
  Store(header.data() + 24, Crc(payload));
  Store(header.data() + 28, Crc(std::span(header).first(28)));
  auto error = WriteAll(fd, header);
  if (error == 0) {
    error = WriteAll(fd, payload);
  }
  if (error == 0) {
    error = SyncFile(fd);
  }
  if (::close(fd) != 0 && error == 0) {
    error = errno;
  }
  if (error == 0 &&
      ::renameat(directory_fd_, temporary, directory_fd_, name) != 0) {
    error = errno;
  }
  if (error == 0) {
    error = SyncDirectory(directory_fd_);
  }
  if (error != 0) {
    (void)::unlinkat(directory_fd_, temporary, 0);
    return IoError(error);
  }
  return {};
}

base::Status Journal::LoadFile(const char* name, std::span<std::byte> storage,
                               size_t& size) const noexcept {
  if (directory_fd_ < 0) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  return LoadFileAt(directory_fd_, name, storage, size);
}

}  // namespace yquant::tradingd
