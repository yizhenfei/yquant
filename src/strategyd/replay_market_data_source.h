#ifndef YQUANT_STRATEGYD_REPLAY_MARKET_DATA_SOURCE_H_
#define YQUANT_STRATEGYD_REPLAY_MARKET_DATA_SOURCE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "strategyd/market_data_source.h"

namespace yquant::strategyd {

// Bounds temporary CSV input storage, excluding its extra overflow-check byte.
inline constexpr std::size_t kMaxReplayFileBytes = 16 * 1024 * 1024;
// Bounds decoded frame storage allocated during Prepare.
inline constexpr std::size_t kMaxReplayFrames = 100000;

// Replays a bounded, single-instrument CSV on the host clock. Prepare reads and
// validates the entire file; Start schedules its owned frames relative to now.
// Equal elapsed timestamps preserve file order. Poll copies one due frame
// without file I/O or allocation, then consumes the optional tail on a later
// call. All access is serialized on the owning strategy thread.
class ReplayMarketDataSource final : public MarketDataSource {
 public:
  // Loads a validated adapter=replay configuration with an absolute file path.
  // May allocate and perform file I/O. Failure leaves an unprepared source
  // unprepared, preserving only its diagnostic; success is single-use.
  [[nodiscard]] base::Status Prepare(
      const md::MarketDataConfig& config) noexcept override;
  // Accepts no subscriptions or exactly the configured instrument. Uses elapsed
  // time divided by replay speed with integer truncation. Invalid selection or
  // deadline overflow preserves prepared state. An empty selection schedules
  // completion at the final frame's scheduled time plus tail.
  [[nodiscard]] base::Status Start(
      std::span<const model::InstrumentId> subscriptions,
      std::uint64_t now) noexcept override;
  // Delivers due frames with receive_time_ns set to now. Even with a zero tail,
  // completion occurs on a subsequent due Poll after the last frame: that call
  // sets finished, clears the deadline, and returns kIdle without changing out.
  [[nodiscard]] MarketDataPollResult Poll(
      std::uint64_t now, model::QuoteTick& out) noexcept override;
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    return deadline_;
  }
  bool finished() const noexcept override { return finished_; }
  // Frees decoded frames and clears the deadline, preserving finished and the
  // last diagnostic. Repeated calls succeed; preparation cannot be repeated.
  [[nodiscard]] base::Status Stop() noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }

 private:
  // Owns one quote and its speed-scaled offset from Start's monotonic origin.
  struct Frame {
    std::uint64_t elapsed_ns = 0;
    model::QuoteTick quote;
  };
  // Stop is terminal; Prepare and Start failures preserve their input state.
  enum class State { kUnprepared, kPrepared, kActive, kStopped };

  // Copies non-null, null-terminated detail into bounded storage and returns
  // code. Does not change lifecycle state or frame storage.
  base::Status Error(base::ErrorCode code, const char* detail) noexcept;
  // Reads and validates config's file into temporary storage, retaining no
  // config references. Success replaces frames and count; failure preserves
  // them and records a diagnostic.
  [[nodiscard]] base::Status ReadFrames(
      const md::MarketDataConfig& config) noexcept;

  State state_ = State::kUnprepared;
  std::unique_ptr<Frame[]> frames_;
  std::size_t frame_count_ = 0;
  // Index of the next frame; frame_count_ means only the tail remains.
  std::size_t next_frame_ = 0;
  model::InstrumentId instrument_id_;
  std::uint64_t tail_ns_ = 0;  // Unscaled delay after the last scheduled frame.
  std::uint64_t start_ns_ = 0;   // Absolute monotonic playback origin.
  std::uint64_t finish_ns_ = 0;  // Absolute last-frame time plus tail.
  bool subscribed_ = false;
  bool finished_ = false;
  std::optional<std::uint64_t> deadline_;
  char error_detail_[768] = {};
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_REPLAY_MARKET_DATA_SOURCE_H_
