#ifndef YQUANT_STRATEGYD_SHM_MARKET_DATA_SOURCE_H_
#define YQUANT_STRATEGYD_SHM_MARKET_DATA_SOURCE_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <optional>
#include <span>

#include "strategyd/market_data_source.h"
#include "yquant/market/shm_broadcast.h"

namespace yquant::strategyd {

// Discovery and heartbeat inspection interval: 250 ms in host nanoseconds.
inline constexpr uint64_t kShmHealthCheckIntervalNs = 250000000;

// Receives subscribed quotes from a shared-memory broadcast stream. Prepare
// copies configuration; Start attaches at Live after subscription registration.
// Poll filters and copies at most one quote using bounded scan/retry budgets.
// All calls are serialized on the owning strategy thread.
//
// Gaps, generation changes, and invalid frames terminate the session without
// automatic reconnect or skipping. Service checks publisher health even without
// subscriptions. Stop releases the reader and preserves failure diagnostics.
class ShmMarketDataSource final : public MarketDataSource {
 public:
  ShmMarketDataSource() = default;
  ShmMarketDataSource(const ShmMarketDataSource&) = delete;
  ShmMarketDataSource& operator=(const ShmMarketDataSource&) = delete;

  // Validates and copies an adapter=shm configuration without attaching. May
  // succeed only once; failure leaves state and stored configuration unchanged.
  [[nodiscard]] base::Status Prepare(
      const market::MarketDataConfig& config) noexcept override;
  // Rejects unknown or duplicate subscriptions before attachment. An open
  // failure preserves prepared state for retry. An empty selection still
  // attaches and schedules health checks, but has no quote-polling deadline.
  [[nodiscard]] base::Status Start(
      std::span<const model::InstrumentId> subscriptions,
      uint64_t now) noexcept override;
  // Sets delivered receive_time_ns to now. Empty reads defer the next attempt
  // by poll_interval_ns; retry or exhausted scan budgets remain immediately
  // eligible but yield the current host batch. Terminal failures are retained
  // for later Poll and Service calls until Stop; non-events preserve out.
  [[nodiscard]] MarketDataPollResult Poll(
      uint64_t now, model::QuoteTick& out) noexcept override;
  std::optional<uint64_t> next_deadline() const noexcept override {
    return deadline_;
  }
  bool is_poll_driven() const noexcept override { return true; }
  // Refreshes discovery and health when the maintenance deadline is due. A
  // replacement generation fails immediately. Missing, stale, or stopped
  // publishers make Poll eligible; it reports health failure only when a read
  // yields no event, allowing retained quotes to be delivered first.
  [[nodiscard]] base::Status Service(uint64_t now) noexcept override;
  std::optional<uint64_t> next_service_deadline() const noexcept override {
    return service_deadline_;
  }
  // Closes the reader and clears polling/maintenance deadlines. Repeated calls
  // succeed and preserve diagnostic text; another Prepare is not allowed.
  [[nodiscard]] base::Status Stop() noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }

 private:
  // Failed suppresses further reads; Stop is terminal for this source object.
  enum class State { kUnprepared, kPrepared, kActive, kStopped, kFailed };

  // Copies non-null C-string detail, truncating if necessary, and returns
  // status unchanged. Does not change lifecycle state or owned resources.
  base::Status Error(base::Status status, const char* detail) noexcept;
  // Enters kFailed, stores status and non-null C-string detail, and clears both
  // deadlines. Called on the failure transition; later reads reuse this result.
  MarketDataPollResult PollError(base::Status status,
                                 const char* detail) noexcept;

  State state_ = State::kUnprepared;
  market::MarketDataConfig config_;
  market::ShmReader reader_;
  market::EventBuffer buffer_;
  // Populated prefix is the subscription selection copied at Start.
  std::array<uint64_t, market::kMaxMarketDataInstruments> instruments_{};
  size_t instrument_count_ = 0;
  std::optional<uint64_t> deadline_;  // Absolute quote-poll time, or idle.
  std::optional<uint64_t> service_deadline_;  // Absolute health-check time.
  market::RefreshResult
      health_;  // Last observation; Poll drains before reporting it.
  base::Status terminal_status_;  // Non-OK while state_ is kFailed.
  char error_detail_[768] = {};
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_SHM_MARKET_DATA_SOURCE_H_
