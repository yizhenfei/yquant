#ifndef YQUANT_STRATEGYD_SHM_MARKET_DATA_SOURCE_H_
#define YQUANT_STRATEGYD_SHM_MARKET_DATA_SOURCE_H_

#include <cstdint>
#include <optional>
#include <span>

#include "strategyd/market_data_source.h"
#include "yquant/md/shm_pubsub.h"

namespace yquant::strategyd {

inline constexpr std::uint64_t kShmHealthCheckIntervalNs = 250000000;

// Main-thread SHM quote adapter. Prepare only copies configuration; Start
// attaches after strategy subscriptions are complete and starts at Live.
// Poll copies at most one quote, using bounded subscriber scan/retry budgets.
// Gaps, generation changes and invalid frames stop delivery until destruction.
class ShmMarketDataSource final : public MarketDataSource {
 public:
  [[nodiscard]] base::Status Prepare(
      const MarketDataConfig& config) noexcept override;
  // Unknown/duplicate subscriptions fail without attaching. Open failure keeps
  // the source prepared, allowing Start to be retried while the publisher
  // starts.
  [[nodiscard]] base::Status Start(
      std::span<const model::InstrumentId> subscriptions,
      std::uint64_t now) noexcept override;
  // Empty reads honor poll_interval_ns; zero continuously polls.
  // receive_time_ns is assigned from this call's now, in the strategy host's
  // clock domain.
  [[nodiscard]] MarketDataPollResult Poll(
      std::uint64_t now, model::QuoteTick& out) noexcept override;
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    return deadline_;
  }
  bool is_poll_driven() const noexcept override { return true; }
  // Refreshes discovery and publisher heartbeat every 250 ms. A replacement
  // generation fails immediately. Stale/missing publishers are remembered so
  // Poll drains committed events before returning a terminal error.
  [[nodiscard]] base::Status Service(std::uint64_t now) noexcept override;
  std::optional<std::uint64_t> next_service_deadline() const noexcept override {
    return service_deadline_;
  }
  [[nodiscard]] base::Status Stop() noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }

 private:
  enum class State { kUnprepared, kPrepared, kActive, kStopped, kFailed };

  // Records a nonterminal operation error without changing owned resources.
  base::Status Error(base::Status status, const char* detail) noexcept;
  // Records the first terminal polling failure, clears readiness and preserves
  // caller output. Stop retains this diagnostic.
  MarketDataPollResult PollError(base::Status status,
                                 const char* detail) noexcept;

  State state_ = State::kUnprepared;
  MarketDataConfig config_;
  md::ShmSubscriber subscriber_;
  md::EventBuffer buffer_;
  std::optional<std::uint64_t> deadline_;
  std::optional<std::uint64_t> service_deadline_;
  md::RefreshResult health_;
  base::Status terminal_status_;
  char error_detail_[768] = {};
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_SHM_MARKET_DATA_SOURCE_H_
