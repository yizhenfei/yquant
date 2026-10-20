#ifndef YQUANT_STRATEGYD_STRATEGY_CONTEXT_H_
#define YQUANT_STRATEGYD_STRATEGY_CONTEXT_H_

#include <cstdint>

#include "strategyd/monotonic_clock.h"
#include "yquant/strategy/strategy_context.h"

namespace yquant::strategyd {

// Callback view of independently owned host services. Owns neither state nor
// resources; the runner serializes access and keeps both dependencies alive.
class StrategyContextImpl final : public strategy::StrategyContext {
 public:
  // Borrows clock and both services for this view's entire lifetime. No
  // reference returned to a strategy may outlive the callback in which it was
  // obtained.
  StrategyContextImpl(const MonotonicClock& clock,
                      strategy::MarketData& market_data,
                      strategy::Trading& trading) noexcept
      : clock_(clock), market_data_(market_data), trading_(trading) {}

  std::uint64_t Now() const noexcept override { return clock_.Now(); }
  strategy::MarketData& market_data() noexcept override { return market_data_; }
  const strategy::MarketData& market_data() const noexcept override {
    return market_data_;
  }
  strategy::Trading& trading() noexcept override { return trading_; }
  const strategy::Trading& trading() const noexcept override {
    return trading_;
  }

 private:
  const MonotonicClock& clock_;
  strategy::MarketData& market_data_;
  strategy::Trading& trading_;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_STRATEGY_CONTEXT_H_
