#ifndef YQUANT_STRATEGYD_MOCK_MARKET_DATA_SOURCE_H_
#define YQUANT_STRATEGYD_MOCK_MARKET_DATA_SOURCE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "strategyd/market_data_source.h"

namespace yquant::strategyd {

// Main-thread mock adapter: one interval schedules a bounded batch, and each
// Poll emits at most one subscribed quote in configuration order. Owns all
// configuration and subscriptions; creates no thread and performs no waits.
class MockMarketDataSource final : public MarketDataSource {
 public:
  [[nodiscard]] base::Status Prepare(
      const MarketDataConfig& config) noexcept override;
  [[nodiscard]] base::Status Start(
      std::span<const model::InstrumentId> subscriptions,
      std::uint64_t now) noexcept override;
  [[nodiscard]] MarketDataPollResult Poll(
      std::uint64_t now, model::QuoteTick& out) noexcept override;
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    return deadline_;
  }
  [[nodiscard]] base::Status Stop() noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }

 private:
  enum class State { kUnprepared, kPrepared, kActive, kStopped, kFailed };

  // Copies the diagnostic and returns code without changing state or output.
  base::Status Error(base::ErrorCode code, const char* detail) noexcept;
  // Records terminal polling failure, clears the deadline, and keeps out
  // intact.
  MarketDataPollResult PollError(base::ErrorCode code,
                                 const char* detail) noexcept;

  State state_ = State::kUnprepared;
  MarketDataConfig config_;
  std::array<std::size_t, kMaxMarketDataInstruments> subscribed_indices_ = {};
  std::array<std::uint64_t, kMaxMarketDataInstruments> sequences_ = {};
  std::size_t subscription_count_ = 0;
  std::size_t next_index_ = 0;
  bool shifted_batch_ = false;
  std::optional<std::uint64_t> deadline_;
  char error_detail_[768] = {};
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_MOCK_MARKET_DATA_SOURCE_H_
