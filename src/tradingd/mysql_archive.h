#ifndef YQUANT_TRADINGD_MYSQL_ARCHIVE_H_
#define YQUANT_TRADINGD_MYSQL_ARCHIVE_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <memory>

#include "tradingd/config.h"

namespace yquant::tradingd {

inline constexpr uint64_t kArchiveFormatVersion = 1;
inline constexpr size_t kArchiveIdentityBytes = 32 * 1024;

// Durable per-run input. Files are written only on owner-thread cold paths.
struct ArchiveRun {
  uint64_t run = 0;
  uint64_t previous = 0;
  uint64_t previous_end = 0;
  bool history_gap = false;
};

// Names are fixed ASCII; output has room for the complete name and terminator.
void ArchiveName(char (&out)[96], const char* kind, uint64_t id) noexcept;
// Finds the lowest retained run greater than after, or zero if none exists.
// Directory errors preserve out. Does not open or lock a Journal.
[[nodiscard]] base::Status NextArchiveRun(const char* directory, uint64_t after,
                                          uint64_t& out) noexcept;
// Validates the immutable v1 run manifest. Failure preserves out.
[[nodiscard]] base::Status ReadArchiveRun(const char* directory, uint64_t run,
                                          ArchiveRun& out) noexcept;

// All database and disk reads belong to one worker. Owner publishes applied
// boundaries only after a complete service iteration. Journal and immutable
// config outlive Stop. No method on the trading path waits for this worker.
class MysqlArchive {
 public:
  MysqlArchive(const MysqlArchive&) = delete;
  MysqlArchive& operator=(const MysqlArchive&) = delete;

  MysqlArchive() noexcept;
  ~MysqlArchive() noexcept;
  [[nodiscard]] base::Status Start(const ServerConfig& config,
                                   const Journal& journal,
                                   uint64_t run) noexcept;
  void PublishApplied(uint64_t lsn) noexcept;
  // Cold shutdown: finish=true attempts to catch up until shutdown_ms; publish
  // the final applied boundary before calling and do not append afterwards.
  // false requests cancellation. Always joins before releasing dependencies.
  void Stop(bool finish) noexcept;
  bool disk_full() const noexcept;
  uint64_t archived_run() const noexcept;
  uint64_t archived_lsn() const noexcept;
  bool failed() const noexcept;

 private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
};
}  // namespace yquant::tradingd
#endif  // YQUANT_TRADINGD_MYSQL_ARCHIVE_H_
