#ifndef YQUANT_MARKETD_REPLAY_SOURCE_H_
#define YQUANT_MARKETD_REPLAY_SOURCE_H_

#include <stddef.h>
#include <stdint.h>

#include "marketd/file_config.h"
#include "marketd/pipeline.h"
#include "marketd/replay_data.h"

namespace yquant::marketd {

// Preloads and validates a bounded file before serving quotes in file order.
// Poll performs no allocation or file I/O. All calls are serialized by the
// daemon thread; receive timestamps belong to that thread's local clock.
class ReplaySource final : public Source {
 public:
  ReplaySource() = default;
  ReplaySource(const ReplaySource&) = delete;
  ReplaySource& operator=(const ReplaySource&) = delete;

  // Replaces any prior replay. Failure leaves the source closed and preserves
  // its diagnostic until the next Init. Positive speed scales offsets with
  // sub-nanosecond fractions rounded down; zero emits without pacing.
  [[nodiscard]] base::Status Init(const ReplayConfig& config) noexcept;

  // The first poll anchors the replay clock, including start_delay_ns. now must
  // not decrease before EOF. Idle, EOF and error preserve out; EOF repeats
  // until Close or Init. Only receive_time_ns is replaced when an event is
  // emitted.
  [[nodiscard]] PollResult Poll(uint64_t now,
                                model::QuoteTick& out) noexcept override;
  void Close() noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }

 private:
  // Keeps the first terminal error without changing the loaded data or cursor.
  PollResult Fail(base::Status status, const char* detail) noexcept;

  ReplayData data_;
  size_t index_ = 0;
  uint64_t start_time_ = 0;
  uint64_t last_time_ = 0;
  uint64_t start_delay_ = 0;
  base::Status failure_;
  bool active_ = false;
  bool started_ = false;
  char error_detail_[256] = {};
};

}  // namespace yquant::marketd

#endif  // YQUANT_MARKETD_REPLAY_SOURCE_H_
