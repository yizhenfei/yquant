#ifndef YQUANT_STRATEGYD_REPLAY_MARKET_DATA_SOURCE_H_
#define YQUANT_STRATEGYD_REPLAY_MARKET_DATA_SOURCE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "strategyd/market_data_source.h"

namespace yquant::strategyd {

inline constexpr std::size_t kMaxReplayFileBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaxReplayFrames = 100000;

// Single-instrument finite source. Prepare cold-reads one bounded CSV and owns
// validated fixed frame storage. Poll copies one due frame without I/O or heap
// work. Equal elapsed timestamps retain file order; timestamps cannot regress.
// All methods run serially on the owning strategy thread.
class ReplayMarketDataSource final : public MarketDataSource {
 public:
  // Requires adapter=replay and an absolute path. Failure leaves an untouched
  // source unprepared, retains diagnostics, and frees all temporary storage.
  [[nodiscard]] base::Status Prepare(
      const MarketDataConfig& config) noexcept override;
  // Accepts no subscriptions or the single configured instrument. Schedules
  // relative to now with integer elapsed/speed truncation. Failure preserves
  // prepared state. Even without subscribers, EOF/tail still finish normally.
  [[nodiscard]] base::Status Start(
      std::span<const model::InstrumentId> subscriptions,
      std::uint64_t now) noexcept override;
  // Last-frame delivery precedes completion. A subsequent due Poll consumes
  // the tail deadline, becomes finished and returns Idle with no deadline.
  // All non-event outcomes preserve out.
  [[nodiscard]] MarketDataPollResult Poll(
      std::uint64_t now, model::QuoteTick& out) noexcept override;
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    return deadline_;
  }
  bool finished() const noexcept override { return finished_; }
  [[nodiscard]] base::Status Stop() noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }

 private:
  struct Frame {
    std::uint64_t elapsed_ns = 0;
    model::QuoteTick quote;
  };
  enum class State { kUnprepared, kPrepared, kActive, kStopped };

  // Copies diagnostics without changing lifecycle state or caller outputs.
  base::Status Error(base::ErrorCode code, const char* detail) noexcept;
  // Reads/validates into temporary owned storage, committing only on success.
  [[nodiscard]] base::Status ReadFrames(
      const MarketDataConfig& config) noexcept;

  State state_ = State::kUnprepared;
  std::unique_ptr<Frame[]> frames_;
  std::size_t frame_count_ = 0;
  std::size_t next_frame_ = 0;
  model::InstrumentId instrument_id_;
  std::uint64_t tail_ns_ = 0;
  std::uint64_t start_ns_ = 0;
  std::uint64_t finish_ns_ = 0;
  bool subscribed_ = false;
  bool finished_ = false;
  std::optional<std::uint64_t> deadline_;
  char error_detail_[768] = {};
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_REPLAY_MARKET_DATA_SOURCE_H_
