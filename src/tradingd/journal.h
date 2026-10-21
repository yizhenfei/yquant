#ifndef YQUANT_TRADINGD_JOURNAL_H_
#define YQUANT_TRADINGD_JOURNAL_H_

#include <pthread.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "yquant/base/status.h"

namespace yquant::tradingd {

inline constexpr std::size_t kJournalMaxPayloadBytes = 16 * 1024;
inline constexpr std::size_t kJournalMaxSnapshotBytes = 64 * 1024 * 1024;
inline constexpr std::size_t kJournalDefaultSegmentBytes = 64 * 1024 * 1024;
inline constexpr std::size_t kJournalMinSegmentBytes = 64 * 1024;
inline constexpr std::size_t kJournalMaxSegmentBytes = 1024 * 1024 * 1024;
inline constexpr std::size_t kJournalMinReserveSegments = 2;
inline constexpr std::size_t kJournalMaxReserveSegments = 16;
inline constexpr std::size_t kJournalDefaultReserveSegments = 2;
inline constexpr std::uint32_t kJournalDefaultSyncIntervalMs = 10;
inline constexpr std::uint32_t kJournalMinSyncIntervalMs = 1;
inline constexpr std::uint32_t kJournalMaxSyncIntervalMs = 1000;
inline constexpr std::size_t kJournalDefaultSyncAfterBytes = 1024 * 1024;
inline constexpr std::size_t kJournalMinSyncAfterBytes = 4096;

// Owned by the caller until Acquire returns; an open descriptor retains the
// directory afterwards. Acquire can create the directory, so callers requiring
// explicit initialization check control metadata before choosing
// initialization.
struct JournalOptions {
  const char* directory = nullptr;
  std::size_t segment_bytes = kJournalDefaultSegmentBytes;
  std::size_t reserve_segments = kJournalDefaultReserveSegments;
  std::uint32_t sync_interval_ms = kJournalDefaultSyncIntervalMs;
  std::size_t sync_after_bytes = kJournalDefaultSyncAfterBytes;
};

// Optional deterministic background-flush hook. A nonnull hook replaces both
// msync and file sync for WAL prefixes; it must implement those operations when
// returning success (zero). The context outlives Close. Segment preparation and
// control-file synchronization always use real OS calls.
struct JournalIoHooks {
  void* context = nullptr;
  int (*flush)(void*, int fd, void* mapping,
               std::size_t bytes) noexcept = nullptr;
};

// The callback borrows payload only until return. Returning failure stops
// replay immediately. A valid record never causes broker sends or callbacks.
using JournalReplayCallback =
    base::Status (*)(void*, std::uint16_t type, std::uint64_t lsn,
                     std::span<const std::byte> payload) noexcept;

struct JournalReplayResult {
  std::uint64_t last_lsn = 0;
  std::uint64_t records = 0;
  // True for an incomplete/corrupt record, missing continuation, or LSN gap.
  // An ordinary zero-filled tail is valid and leaves this false.
  bool tail_discarded = false;
};

// Position immediately after a completely applied transaction. Snapshot owners
// persist this cursor with the state that covers lsn. It is not a durability
// acknowledgment; synchronize the snapshot before publishing its pointer.
struct JournalCursor {
  std::uint64_t stream_id = 0;
  std::uint64_t segment = 0;
  std::uint64_t offset = 0;
  std::uint64_t lsn = 0;
};

// One owning thread acquires the directory lock, replays old streams and starts
// a fresh stream. Thereafter only that thread calls TryAppend. Appending writes
// MAP_SHARED pages without allocation, mutexes, file syscalls or sync waits.
// The private worker prepares spare mappings and flushes published prefixes.
// Successful append is CPU publication, not a physical-durability guarantee.
// Methods other than watermarks/healthy and SaveFile are serialized by owner.
class Journal {
 public:
  Journal() noexcept = default;
  ~Journal() noexcept;
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  // Acquires stable LOCK using nonblocking flock. Failure releases resources;
  // a successful call holds LOCK until Close, including recovery.
  [[nodiscard]] base::Status Acquire(
      const JournalOptions& options,
      const JournalIoHooks* hooks = nullptr) noexcept;
  // Scans a stream from LSN 1 and stops at the first invalid/unpublished tail,
  // without skipping corruption. Corrupt required segment headers are fatal.
  // Failure may have delivered earlier valid records; result is unchanged.
  [[nodiscard]] base::Status Replay(std::uint64_t stream_id,
                                    JournalReplayCallback callback,
                                    void* context,
                                    JournalReplayResult& result) noexcept;
  // Starts at a cursor stored in a validated snapshot. Earlier covered
  // records/segments are not inspected. The anchor header must remain valid;
  // the first subsequent business record must have cursor.lsn + 1.
  [[nodiscard]] base::Status ReplayFrom(const JournalCursor& cursor,
                                        JournalReplayCallback callback,
                                        void* context,
                                        JournalReplayResult& result) noexcept;
  // Exclusively creates a stream. Prepares active and spare mappings before
  // returning. Stream identifiers must never be reused, including empty runs.
  [[nodiscard]] base::Status Start(std::uint64_t stream_id) noexcept;
  // Type zero is reserved for SegmentEnd. Failure leaves lsn unchanged and
  // does not publish any part of this record. Exhaustion is retryable while
  // healthy; callers must stop new risk while awaiting prepared capacity.
  [[nodiscard]] base::Status TryAppend(std::uint16_t type,
                                       std::span<const std::byte> payload,
                                       std::uint64_t& lsn) noexcept;
  // Owner-thread only, after Start and at a fully applied transaction boundary.
  // Do not capture state between append publication and applying its mutation.
  JournalCursor GetCursor() const noexcept;
  // Cold-path wait until the prefix published at entry is synced. No append
  // calls may run concurrently with Flush or Close.
  [[nodiscard]] base::Status Flush() noexcept;
  // Stops and joins worker, flushes, releases all mappings and LOCK. Returns
  // the first retained failure. Repeated Close returns the same status.
  [[nodiscard]] base::Status Close() noexcept;

  // Atomic CRC-checked opaque files in the locked directory. Names contain
  // only letters, digits, '.', '_' or '-' and cannot be '.' or '..'. SaveFile
  // writes/syncs a temporary, renames and syncs the directory. It may run on
  // one checkpoint worker concurrently with WAL appends/background flushes;
  // the caller keeps payload immutable and joins that worker before Close.
  // SaveFile and LoadFile calls must be serialized with one another. These
  // operations do not alter the retained WAL error diagnostic on failure.
  [[nodiscard]] base::Status SaveFile(
      const char* name, std::span<const std::byte> payload) const noexcept;
  // Validates length against storage and kJournalMaxSnapshotBytes before read.
  // Failure preserves size; storage contents are unspecified on failure.
  [[nodiscard]] base::Status LoadFile(const char* name,
                                      std::span<std::byte> storage,
                                      std::size_t& size) const noexcept;

  bool healthy() const noexcept;
  const char* error_detail() const noexcept;
  std::uint64_t published_lsn() const noexcept {
    return published_lsn_.load(std::memory_order_acquire);
  }
  std::uint64_t durable_lsn() const noexcept {
    return durable_lsn_.load(std::memory_order_acquire);
  }

 private:
  enum class SegmentState : std::uint8_t { kFree, kReady, kActive, kSealed };
  // State transfers ownership: worker initializes a free slot then publishes
  // Ready; owner publishes Active/Sealed; worker alone reclaims sealed slots.
  struct Segment {
    std::atomic<SegmentState> state{SegmentState::kFree};
    std::uint64_t sequence = 0;
    int fd = -1;
    std::byte* mapping = nullptr;
    std::atomic<std::size_t> published_bytes{0};
    std::atomic<std::uint64_t> published_lsn{0};
    std::size_t synced_bytes = 0;
    std::uint64_t synced_lsn = 0;
  };
  // Ring capacity exceeds maximum ready reservations plus current and dirty
  // segments. Delayed I/O exhausts bounded slots rather than allocating.
  static constexpr std::size_t kSegmentSlots = 32;
  // Owns mapped spare preparation, ordered synchronization and sealed reclaim.
  static void* ThreadEntry(void* context) noexcept;
  void RunWorker() noexcept;
  // Initializes only a free slot, publishing Ready after header durability.
  // Returns EBUSY for bounded ring exhaustion; other errno values are terminal.
  int PrepareSegment(std::uint64_t sequence) noexcept;
  // Acknowledges at most the prefix captured before synchronization starts.
  int SyncSegment(Segment& segment, bool sealed) noexcept;
  // Reclaims a slot after all worker references finish; keeps the first errno.
  int ReleaseSegment(Segment& segment) noexcept;
  // Keeps the first owner-thread error and publishes unhealthy atomically.
  base::Status Fail(base::Status status, const char* detail) noexcept;
  // Imports a worker errno into the retained owner-thread diagnostic.
  base::Status WorkerFailure() noexcept;

  std::array<Segment, kSegmentSlots> segments_{};
  JournalOptions options_{};
  JournalIoHooks hooks_{};
  int directory_fd_ = -1;
  int lock_fd_ = -1;
  int stream_fd_ = -1;
  pthread_t worker_{};
  bool worker_started_ = false;
  std::uint64_t stream_id_ = 0;
  std::uint64_t current_sequence_ = 0;
  std::size_t current_offset_ = 0;
  std::atomic<std::uint64_t> active_sequence_{0};
  std::atomic<std::uint64_t> published_lsn_{0};
  std::atomic<std::uint64_t> durable_lsn_{0};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> force_flush_{false};
  std::atomic<int> worker_error_{0};
  std::atomic<bool> failed_{false};
  std::atomic<bool> acquired_{false};
  base::Status failure_{};
  char error_detail_[256] = {};
};

}  // namespace yquant::tradingd

#endif  // YQUANT_TRADINGD_JOURNAL_H_
