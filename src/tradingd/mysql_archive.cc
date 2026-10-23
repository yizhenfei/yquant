#include "tradingd/mysql_archive.h"

#include <pthread.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <thread>

#include "tradingd/archive_projection.h"
#include "tradingd/state_codec.h"

namespace yquant::tradingd {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kDiskSpaceCheckInterval = std::chrono::milliseconds(100);
base::Status Corrupt() noexcept { return {base::ErrorCode::kCorrupt, {}}; }

// File names must match exactly; temporary files never enter the run chain.
bool ParseName(std::string_view name, const char* kind,
               std::uint64_t& id) noexcept {
  char prefix[48];
  std::snprintf(prefix, sizeof(prefix), "archive-%s-", kind);
  const auto n = std::strlen(prefix);
  if (!name.starts_with(prefix) || name.size() != n + 24 ||
      name.substr(n + 20) != ".bin")
    return false;
  std::uint64_t value = 0;
  for (const char ch : name.substr(n, 20)) {
    if (ch < '0' || ch > '9' || value > (UINT64_MAX - (ch - '0')) / 10)
      return false;
    value = value * 10 + ch - '0';
  }
  id = value;
  return value != 0;
}

// Selects one file in bounded memory even after many restarts.
base::Status FindFile(const char* directory, const char* kind,
                      std::uint64_t after, bool latest,
                      std::uint64_t& out) noexcept {
  std::error_code error;
  std::uint64_t found = 0;
  for (std::filesystem::directory_iterator it(directory, error), end;
       !error && it != end; it.increment(error)) {
    std::uint64_t id = 0;
    if (ParseName(it->path().filename().native(), kind, id) && id > after &&
        (found == 0 || (latest ? id > found : id < found)))
      found = id;
  }
  if (error) return {base::ErrorCode::kIoError, error};
  out = found;
  return {};
}
}  // namespace

void ArchiveName(char (&out)[96], const char* kind, std::uint64_t id) noexcept {
  std::snprintf(out, sizeof(out), "archive-%s-%020llu.bin", kind,
                static_cast<unsigned long long>(id));
}
base::Status NextArchiveRun(const char* directory, std::uint64_t after,
                            std::uint64_t& out) noexcept {
  return FindFile(directory, "run", after, false, out);
}
base::Status ReadArchiveRun(const char* directory, std::uint64_t run,
                            ArchiveRun& out) noexcept {
  char name[96];
  ArchiveName(name, "run", run);
  std::byte bytes[64];
  std::size_t size = 0;
  auto status = ReadJournalFile(directory, name, bytes, size);
  if (!status.ok()) return status;
  StateReader reader{std::span(bytes).first(size)};
  if (reader.U64() != kArchiveFormatVersion) return Corrupt();
  ArchiveRun candidate;
  candidate.run = reader.U64();
  candidate.previous = reader.U64();
  candidate.previous_end = reader.U64();
  const auto gap = reader.U64();
  candidate.history_gap = gap != 0;
  if (!reader.done() || candidate.run != run || candidate.previous >= run ||
      gap > 1 || (candidate.previous == 0 && candidate.previous_end != 0))
    return Corrupt();
  out = candidate;
  return {};
}

struct MysqlArchive::Storage {
  ServerConfig config;
  const Journal* journal = nullptr;
  std::uint64_t live_run = 0;
  std::atomic<std::uint64_t> applied{0}, archived_run{0}, archived_lsn{0};
  std::atomic<bool> stop{false}, finish{false}, failed{false}, disk_full{false};
  std::atomic<std::int64_t> deadline_ns{0};
  pthread_t thread{};
  bool started = false;
  base::SteadyMonotonicClock clock;
  std::unique_ptr<ArchiveProjection> projection;
  std::unique_ptr<std::byte[]> baseline;
  std::size_t baseline_size = 0;
  MysqlArchiveStore store;
  ArchiveRun run;
  JournalCursor cursor;
  bool loaded = false;
  bool connected = false;
  bool permanent = false;
  Clock::time_point next_disk_check{};

  static bool Cancelled(void* context) noexcept {
    return static_cast<Storage*>(context)->ShouldStop();
  }
  bool ShouldStop() const noexcept {
    if (!stop.load(std::memory_order_acquire)) return false;
    if (!finish.load(std::memory_order_acquire)) return true;
    return Clock::now().time_since_epoch().count() >=
           deadline_ns.load(std::memory_order_acquire);
  }
  // Files can accumulate while SQL is unavailable. This independent check also
  // runs during retry/permanent-error waits, with bounded syscall frequency.
  base::Status CheckDiskSpace() noexcept {
    const auto now = Clock::now();
    if (now < next_disk_check) return {};
    next_disk_check = now + kDiskSpaceCheckInterval;
    struct statvfs space{};
    if (::statvfs(config.persistence.directory, &space) != 0)
      return {base::ErrorCode::kIoError,
              std::error_code(errno, std::generic_category())};
    const auto free_bytes =
        static_cast<std::uint64_t>(space.f_bavail) * space.f_frsize;
    disk_full.store(free_bytes < config.mysql_archive.min_free_bytes,
                    std::memory_order_release);
    return {};
  }
  // Retry/idle waits remain cancellable and keep disk monitoring alive.
  void Pause(std::uint32_t ms) noexcept {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (!ShouldStop() && Clock::now() < end) {
      (void)CheckDiskSpace();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  // Loads an atomic immutable identity copy, never the owner's mutable table.
  // Existing session conflicts invalidate this speculative projection.
  base::Status Identity() noexcept {
    std::uint64_t id = 0;
    auto status =
        FindFile(config.persistence.directory, "identity", 0, true, id);
    if (!status.ok()) return status;
    if (id == 0) return Corrupt();
    char name[96];
    ArchiveName(name, "identity", id);
    std::array<std::byte, kArchiveIdentityBytes> bytes;
    std::size_t size = 0;
    status = ReadJournalFile(config.persistence.directory, name, bytes, size);
    if (!status.ok()) return status;
    return projection->LoadIdentity(std::span(bytes).first(size));
  }
  // Replaces the worker's projection with a run's LSN-zero baseline. Failure
  // leaves it unusable until Resume reconstructs it; no SQL has been issued.
  base::Status Load(std::uint64_t id) noexcept {
    auto status = ReadArchiveRun(config.persistence.directory, id, run);
    if (!status.ok()) return status;
    char name[96];
    ArchiveName(name, "base", id);
    status = ReadJournalFile(config.persistence.directory, name,
                             {baseline.get(), kJournalMaxSnapshotBytes},
                             baseline_size);
    if (!status.ok()) return status;
    projection.reset(new (std::nothrow) ArchiveProjection(&clock));
    if (!projection) return {base::ErrorCode::kResourceExhausted, {}};
    status = projection->Configure(config);
    if (status.ok())
      status = projection->Restore({baseline.get(), baseline_size});
    if (status.ok()) status = Identity();
    if (!status.ok()) return status;
    if (projection->run_id() != id) return Corrupt();
    cursor = JournalStartCursor(id);
    return {};
  }
  // Scanner borrows payload for this call; the shadow ledger retains owned
  // fields. Failure requires reconstruction before another batch.
  static base::Status Replay(void* context, std::uint16_t type,
                             std::uint64_t lsn,
                             std::span<const std::byte> payload) noexcept {
    auto& self = *static_cast<Storage*>(context);
    if (self.ShouldStop()) return {base::ErrorCode::kInvalidState, {}};
    return self.projection->Replay(type, lsn, payload);
  }
  // Runs under the database progress lock. Failure leaves speculative changes
  // only; the caller rolls SQL back and reconstructs the shadow ledger.
  static base::Status Ingest(void* context, std::uint16_t type,
                             std::uint64_t lsn,
                             std::span<const std::byte> payload) noexcept {
    auto& self = *static_cast<Storage*>(context);
    auto status = Replay(context, type, lsn, payload);
    if (status.ok())
      status = self.store.AppendEvent(type, lsn, payload, *self.projection);
    return status;
  }
  // Returns the live applied/durable intersection or an immutable old end.
  // Only successful results may be used to admit records into SQL.
  base::Status Limit(std::uint64_t& out) noexcept {
    if (run.run == live_run) {
      out = std::min(applied.load(std::memory_order_acquire),
                     journal->durable_lsn());
      return {};
    }
    char name[96];
    ArchiveName(name, "end", run.run);
    std::byte bytes[32];
    std::size_t size = 0;
    auto status =
        ReadJournalFile(config.persistence.directory, name, bytes, size);
    if (!status.ok()) return status;
    StateReader reader{std::span(bytes).first(size)};
    if (reader.U64() != kArchiveFormatVersion || reader.U64() != run.run)
      return Corrupt();
    out = reader.U64();
    return reader.done() ? base::Status{} : Corrupt();
  }
  // Reconstructs exclusively from local baseline and committed SQL progress,
  // including after an unknown COMMIT outcome. Failure retains no SQL claim.
  base::Status Resume() noexcept {
    std::uint64_t first = 0;
    auto status = NextArchiveRun(config.persistence.directory, 0, first);
    if (!status.ok() || first == 0) return status.ok() ? Corrupt() : status;
    status = Load(first);
    if (!status.ok()) return status;
    if (!connected) {
      status = store.Connect(config.mysql_archive);
      if (!status.ok()) return status;
      connected = true;
    }
    MysqlArchiveProgress progress;
    status = store.GetProgress(*projection, progress);
    if (!status.ok()) return status;
    if (progress.exists) {
      if (!progress.baseline_installed) return Corrupt();
      status = Load(progress.cursor.stream_id);
      std::uint64_t limit = 0;
      if (status.ok()) status = Limit(limit);
      if (status.ok() && progress.cursor.lsn > limit) return Corrupt();
      if (status.ok())
        status = ScanJournal(config.persistence.directory, cursor,
                             progress.cursor.lsn, Replay, this);
      if (!status.ok()) return status;
      if (cursor.segment != progress.cursor.segment ||
          cursor.offset != progress.cursor.offset)
        return Corrupt();
    } else {
      status = Install();
      if (!status.ok()) return status;
    }
    loaded = true;
    return {};
  }
  // Installs LSN zero under the progress lock. Store rechecks the predecessor
  // after locking; a competing writer makes this attempt retry from Resume.
  base::Status Install() noexcept {
    MysqlArchiveProgress locked;
    auto status = store.Begin(*projection, locked);
    if (status.ok())
      status =
          store.InstallRun({baseline.get(), baseline_size}, *projection,
                           run.previous, run.previous_end, run.history_gap);
    if (status.ok()) status = store.Commit(cursor, *projection);
    if (!status.ok()) store.Rollback();
    return status;
  }
  // Advances one bounded batch or one run boundary. Failed speculative state
  // is discarded by Entry; committed cursor changes only after COMMIT succeeds.
  base::Status Step() noexcept {
    if (!loaded) return Resume();
    std::uint64_t limit = 0;
    auto status = Limit(limit);
    if (!status.ok()) return status;
    if (cursor.lsn > limit) return Corrupt();
    if (cursor.lsn == limit) {
      archived_run.store(run.run, std::memory_order_release);
      archived_lsn.store(cursor.lsn, std::memory_order_release);
      if (run.run == live_run) {
        if (stop.load(std::memory_order_acquire)) {
          // Stop publishes the owner's final applied boundary. The limit read
          // above may predate that publication; only the freshly acquired
          // boundary can prove that shutdown has caught up.
          const auto final_applied = applied.load(std::memory_order_acquire);
          if (cursor.lsn == final_applied &&
              journal->durable_lsn() >= final_applied) {
            MysqlArchiveProgress locked;
            status = store.Begin(*projection, locked);
            if (status.ok() && (locked.cursor.stream_id != cursor.stream_id ||
                                locked.cursor.lsn != cursor.lsn)) {
              store.Rollback();
              loaded = false;
              return {};
            }
            if (status.ok())
              status = store.FinalizeRun(*projection, final_applied);
            if (status.ok()) status = store.Commit(cursor, *projection);
            if (!status.ok()) {
              store.Rollback();
              return status;
            }
            finish.store(false, std::memory_order_release);
          } else if (journal->durable_lsn() < final_applied)
            Pause(config.mysql_archive.poll_ms);
        } else {
          Pause(config.mysql_archive.poll_ms);
        }
        return {};
      }
      const auto old_run = run.run;
      const auto old_end = cursor.lsn;
      std::uint64_t next = 0;
      status = NextArchiveRun(config.persistence.directory, old_run, next);
      if (!status.ok() || next == 0) return status.ok() ? Corrupt() : status;
      status = Load(next);
      if (status.ok() &&
          (run.previous != old_run || run.previous_end != old_end))
        return Corrupt();
      if (status.ok()) status = Install();
      return status;
    }
    status = Identity();
    if (!status.ok()) return status;
    MysqlArchiveProgress locked;
    status = store.Begin(*projection, locked);
    if (!status.ok()) return status;
    if (locked.cursor.stream_id != cursor.stream_id ||
        locked.cursor.lsn != cursor.lsn ||
        locked.cursor.segment != cursor.segment ||
        locked.cursor.offset != cursor.offset) {
      store.Rollback();
      loaded = false;
      return {};
    }
    // A worst-case payload bound keeps the batch within its byte budget.
    const auto count = std::min<std::uint64_t>(
        config.mysql_archive.batch_records,
        config.mysql_archive.batch_bytes / kJournalMaxPayloadBytes);
    const auto through = cursor.lsn + std::min(limit - cursor.lsn, count);
    auto candidate = cursor;
    status = ScanJournal(config.persistence.directory, candidate, through,
                         Ingest, this);
    if (status.ok()) status = store.Commit(candidate, *projection);
    if (!status.ok()) {
      store.Rollback();
      return status;
    }
    cursor = candidate;
    archived_run.store(run.run, std::memory_order_release);
    archived_lsn.store(cursor.lsn, std::memory_order_release);
    return {};
  }
  static void* Entry(void* context) noexcept {
    auto& self = *static_cast<Storage*>(context);
    while (!self.ShouldStop()) {
      auto status = self.CheckDiskSpace();
      if (status.ok()) {
        if (self.permanent) {
          self.Pause(100);
          continue;
        }
        status = self.Step();
      }
      if (status.ok()) {
        self.failed.store(false, std::memory_order_release);
        continue;
      }
      self.failed.store(true, std::memory_order_release);
      const bool database_failure = self.store.error_detail()[0] != '\0';
      std::fprintf(stderr,
                   "mysql archive run=%llu lsn=%llu: %s (code %u, %s)\n",
                   static_cast<unsigned long long>(self.run.run),
                   static_cast<unsigned long long>(self.cursor.lsn),
                   database_failure ? self.store.error_detail()
                                    : "archive source read/replay failed",
                   static_cast<unsigned>(status.code),
                   status.system_error.message().c_str());
      self.permanent =
          self.permanent || (database_failure && !self.store.retriable()) ||
          status.code == base::ErrorCode::kCorrupt ||
          status.code == base::ErrorCode::kUnsupported ||
          status.code == base::ErrorCode::kInvalidArgument ||
          (status.code == base::ErrorCode::kInvalidState && !self.ShouldStop());
      self.store.Close();
      self.connected = false;
      self.loaded = false;
      self.Pause(self.config.mysql_archive.retry_ms);
    }
    self.store.Close();
    return nullptr;
  }
};
MysqlArchive::MysqlArchive() noexcept = default;
MysqlArchive::~MysqlArchive() noexcept { Stop(false); }
base::Status MysqlArchive::Start(const ServerConfig& config,
                                 const Journal& journal,
                                 std::uint64_t run) noexcept {
  Stop(false);
  if (!config.mysql_archive.enabled) return {};
  storage_.reset(new (std::nothrow) Storage);
  if (!storage_) return {base::ErrorCode::kResourceExhausted, {}};
  auto& s = *storage_;
  s.config = config;
  s.journal = &journal;
  s.live_run = run;
  s.store.SetCancellation(Storage::Cancelled, &s);
  s.baseline.reset(new (std::nothrow) std::byte[kJournalMaxSnapshotBytes]);
  if (!s.baseline) return {base::ErrorCode::kResourceExhausted, {}};
  const auto error = pthread_create(&s.thread, nullptr, Storage::Entry, &s);
  if (error != 0)
    return {base::ErrorCode::kIoError,
            std::error_code(error, std::generic_category())};
  s.started = true;
  return {};
}
void MysqlArchive::PublishApplied(std::uint64_t lsn) noexcept {
  if (storage_) storage_->applied.store(lsn, std::memory_order_release);
}
void MysqlArchive::Stop(bool finish) noexcept {
  if (!storage_) return;
  auto& s = *storage_;
  s.deadline_ns.store((Clock::now() + std::chrono::milliseconds(
                                          s.config.mysql_archive.shutdown_ms))
                          .time_since_epoch()
                          .count(),
                      std::memory_order_release);
  s.finish.store(finish, std::memory_order_release);
  s.stop.store(true, std::memory_order_release);
  if (s.started) pthread_join(s.thread, nullptr);
  if (finish && (s.archived_run.load() != s.live_run ||
                 s.archived_lsn.load() != s.applied.load()))
    std::fprintf(stderr, "mysql archive has pending history at shutdown\n");
  storage_.reset();
}
bool MysqlArchive::disk_full() const noexcept {
  return storage_ && storage_->disk_full.load(std::memory_order_acquire);
}
bool MysqlArchive::failed() const noexcept {
  return storage_ && storage_->failed.load(std::memory_order_acquire);
}
std::uint64_t MysqlArchive::archived_run() const noexcept {
  return storage_ ? storage_->archived_run.load(std::memory_order_acquire) : 0;
}
std::uint64_t MysqlArchive::archived_lsn() const noexcept {
  return storage_ ? storage_->archived_lsn.load(std::memory_order_acquire) : 0;
}
}  // namespace yquant::tradingd
