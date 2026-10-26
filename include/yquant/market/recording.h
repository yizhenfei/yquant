#ifndef YQUANT_MARKET_RECORDING_H_
#define YQUANT_MARKET_RECORDING_H_

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <atomic>
#include <span>

#include "yquant/base/spsc_queue.h"
#include "yquant/market/file_config.h"
#include "yquant/market/pipeline.h"
#include "yquant/market/replay_data.h"

namespace yquant::market {

inline constexpr size_t kRecordingHeaderSize = 64;
inline constexpr size_t kRecordingRecordSize = 96;

// Loads a sealed version-1 recording, validating every frame and checksum
// before replacing output. Failure preserves output; optional diagnostic
// storage may be empty and is always NUL-terminated when nonempty. Files
// exceeding max_records are rejected before record storage is allocated.
[[nodiscard]] base::Status LoadRecordingFile(
    const char* path, size_t max_records, ReplayData& output,
    std::span<char> error_detail) noexcept;

// Optional backend operations for deterministic I/O failure tests. Null hooks
// use POSIX calls. The context must outlive Finish(); write runs on the backend
// thread, while sync and close run on the owning thread during Finish(). A
// close hook must close its descriptor even when it reports an error.
struct RecordIoHooks {
  void* context = nullptr;
  ssize_t (*write)(void*, int, const void*, size_t) noexcept = nullptr;
  int (*sync)(void*, int) noexcept = nullptr;
  int (*close)(void*, int) noexcept = nullptr;
};

// One producer queues fixed-size frames; a private consumer batches disk
// writes. Init, Publish, Service, Finish and Close must be serialized by the
// host. Publish performs no allocation or I/O. Queue exhaustion and
// asynchronous I/O failures are terminal and leave the recording unsealed.
// Finish drains, joins, seals and syncs the file and its parent directory; its
// result is stable on repeated calls.
// A seal confirms a complete layout. A final sync or close failure may leave a
// sealed file; callers must inspect Finish's result to confirm persistence.
class RecordPublisher final : public Publisher {
 public:
  RecordPublisher() noexcept = default;
  ~RecordPublisher() noexcept override { Close(); }
  RecordPublisher(const RecordPublisher&) = delete;
  RecordPublisher& operator=(const RecordPublisher&) = delete;

  // Creates path exclusively. Parent directories must exist. Failure releases
  // resources and removes an output created by this unsuccessful
  // initialization.
  [[nodiscard]] base::Status Init(
      const RecordConfig& config,
      const RecordIoHooks* hooks = nullptr) noexcept;
  [[nodiscard]] base::Status Publish(
      const model::QuoteTick& quote) noexcept override;
  [[nodiscard]] base::Status Service(uint64_t now) noexcept override;
  [[nodiscard]] base::Status Finish() noexcept override;
  // Best-effort Finish; use Finish directly when shutdown errors matter.
  void Close() noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }

 private:
  // Keeps the first host-thread failure and its diagnostic until the next Init.
  base::Status Fail(base::Status status, const char* detail) noexcept;
  // Transfers the first backend errno into the host-thread diagnostic.
  base::Status CheckWorker() noexcept;
  // Detaches both endpoints after joining; owns queue_memory_.
  void ReleaseStorage() noexcept;
  // pthread entry owns consumer_ until it returns; no exceptions or allocation.
  static void* ThreadEntry(void* context) noexcept;
  void RunWorker() noexcept;

  base::SpscQueue producer_;
  base::SpscQueue consumer_;
  void* queue_memory_ = nullptr;
  pthread_t thread_{};
  std::atomic<bool> stopping_{false};
  std::atomic<int> worker_error_{0};
  RecordIoHooks hooks_;
  int fd_ = -1;
  int directory_fd_ = -1;
  bool active_ = false;
  uint64_t first_receive_time_ns_ = 0;
  uint64_t last_offset_ns_ = 0;
  uint64_t record_count_ = 0;
  base::Status failure_;
  char error_detail_[256] = {};
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_RECORDING_H_
