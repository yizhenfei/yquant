#ifndef YQUANT_STRATEGYD_STRATEGY_CONTEXT_H_
#define YQUANT_STRATEGYD_STRATEGY_CONTEXT_H_

#include <cstdint>

#include "yquant/base/monotonic_clock.h"
#include "yquant/strategy/strategy_context.h"

namespace yquant::strategyd {

// Exposes the host clock and services through StrategyContext during callbacks.
// The runner controls their lifecycle and serializes access on the strategy
// thread. Strategies may use this view only during the current callback.
class StrategyContextImpl final : public strategy::StrategyContext {
 public:
  // Borrows clock and all services until destruction. Returned service
  // references remain callback-scoped and must not be retained by a strategy.
  StrategyContextImpl(const base::MonotonicClock& clock,
                      strategy::MarketData& market_data,
                      strategy::Trading& trading,
                      strategy::Timer& timer) noexcept
      : clock_(clock),
        market_data_(market_data),
        trading_(trading),
        timer_(timer) {}

  std::uint64_t Now() const noexcept override { return clock_.Now(); }
  strategy::MarketData& market_data() noexcept override { return market_data_; }
  const strategy::MarketData& market_data() const noexcept override {
    return market_data_;
  }
  strategy::Trading& trading() noexcept override { return trading_; }
  const strategy::Trading& trading() const noexcept override {
    return trading_;
  }
  strategy::Timer& timer() noexcept override { return timer_; }
  const strategy::Timer& timer() const noexcept override { return timer_; }

 private:
  const base::MonotonicClock& clock_;
  strategy::MarketData& market_data_;
  strategy::Trading& trading_;
  strategy::Timer& timer_;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_STRATEGY_CONTEXT_H_
