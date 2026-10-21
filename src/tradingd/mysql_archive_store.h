#ifndef YQUANT_TRADINGD_MYSQL_ARCHIVE_STORE_H_
#define YQUANT_TRADINGD_MYSQL_ARCHIVE_STORE_H_

#include <cstddef>
#include <cstdint>
#include <span>

#include "tradingd/journal.h"
#include "yquant/base/status.h"

namespace yquant::tradingd {

class ArchiveProjection;

// Owned configuration; passwords are read only from the named environment
// variable. The archive worker owns all database operations and their waits.
struct MysqlArchiveOptions {
  bool enabled = false;
  char host[256] = "127.0.0.1";
  std::uint16_t port = 3306;
  char database[65] = {};
  char user[65] = {};
  char password_env[129] = {};
  char tls_ca[4097] = {};
  std::uint32_t timeout_seconds = 2;
  std::uint32_t batch_records = 256;
  std::size_t batch_bytes = 1024 * 1024;
  std::uint32_t poll_ms = 100;
  std::uint32_t retry_ms = 1000;
  std::uint32_t shutdown_ms = 5000;
  std::uint64_t min_free_bytes = 64 * 1024 * 1024;
};

// No progress exists before the first baseline transaction commits. A present
// row always identifies an installed baseline and a continuous consumed prefix.
struct MysqlArchiveProgress {
  bool exists = false;
  bool baseline_installed = false;
  JournalCursor cursor;
};

// Single-thread-owned MySQL archive connection. Facts, projections, and
// progress share one transaction. Any failed transaction must be rolled back;
// callers discard their speculative projection and resume from persisted
// progress.
class MysqlArchiveStore {
 public:
  MysqlArchiveStore() noexcept = default;
  ~MysqlArchiveStore() noexcept;
  MysqlArchiveStore(const MysqlArchiveStore&) = delete;
  MysqlArchiveStore& operator=(const MysqlArchiveStore&) = delete;

  // Connects with bounded network timeouts, strict SQL mode and schema v1.
  // Failure leaves no usable connection. Builds without MySQL return
  // Unsupported.
  [[nodiscard]] base::Status Connect(
      const MysqlArchiveOptions& options) noexcept;
  void Close() noexcept;
  // Hook is checked before each SQL operation. It must remain valid until
  // Close and runs on the connection's owning thread; nullptr disables it.
  void SetCancellation(bool (*cancelled)(void*) noexcept,
                       void* context) noexcept;
  // Verifies immutable ledger identity. The result is a committed snapshot;
  // Begin must be used to lock and recheck it before changing any data.
  [[nodiscard]] base::Status GetProgress(const ArchiveProjection& projection,
                                         MysqlArchiveProgress& out) noexcept;
  // Starts a transaction and locks the ledger progress. Creates identity and
  // the initial empty progress row atomically. Failure preserves out.
  [[nodiscard]] base::Status Begin(const ArchiveProjection& projection,
                                   MysqlArchiveProgress& out) noexcept;
  // Installs an immutable baseline after its predecessor has been consumed.
  // previous_end is its effective final LSN. history_gap records missing
  // history.
  [[nodiscard]] base::Status InstallRun(std::span<const std::byte> baseline,
                                        const ArchiveProjection& projection,
                                        std::uint64_t previous_run,
                                        std::uint64_t previous_end,
                                        bool history_gap) noexcept;
  // Inserts or checks an exact event fact. Requires projection after Replay;
  // records must follow locked progress continuously within one run.
  [[nodiscard]] base::Status AppendEvent(
      std::uint16_t type, std::uint64_t lsn, std::span<const std::byte> payload,
      const ArchiveProjection& projection) noexcept;
  // Records a known final boundary under the existing progress lock. Repeated
  // calls accept only identical boundaries and never shorten archived history.
  [[nodiscard]] base::Status FinalizeRun(const ArchiveProjection& projection,
                                         std::uint64_t final_lsn) noexcept;
  // Publishes the supplied fully decoded projection and cursor, then commits.
  // A failed COMMIT is retryable with unknown outcome; reconnect and
  // GetProgress before replaying. Successful return alone acknowledges
  // progress.
  [[nodiscard]] base::Status Commit(
      const JournalCursor& cursor,
      const ArchiveProjection& projection) noexcept;
  void Rollback() noexcept;

  bool retriable() const noexcept { return retriable_; }
  const char* error_detail() const noexcept { return error_detail_; }

 private:
  struct Storage;
  [[nodiscard]] base::Status WriteProjection(
      const ArchiveProjection& projection) noexcept;
  [[nodiscard]] base::Status Fail(base::ErrorCode code, const char* detail,
                                  bool retry = false) noexcept;
  [[nodiscard]] base::Status DatabaseFailure() noexcept;
  Storage* storage_ = nullptr;
  bool (*cancelled_)(void*) noexcept = nullptr;
  void* cancellation_context_ = nullptr;
  bool retriable_ = false;
  char error_detail_[256] = {};
};

}  // namespace yquant::tradingd

#endif  // YQUANT_TRADINGD_MYSQL_ARCHIVE_STORE_H_
