#ifndef YQUANT_STRATEGY_STRATEGY_CONTEXT_H_
#define YQUANT_STRATEGY_STRATEGY_CONTEXT_H_

#include <cstdint>

#include "yquant/strategy/market_data.h"
#include "yquant/strategy/timer.h"
#include "yquant/strategy/trading.h"

namespace yquant::strategy {

// Host-owned callback view of time and strategy services. Borrow only during
// the currently executing callback on the strategy thread; do not retain this
// object or pass it to another thread.
class StrategyContext {
 public:
  virtual ~StrategyContext() noexcept = default;

  // Returns nanoseconds elapsed from the host's process-local monotonic base.
  virtual std::uint64_t Now() const noexcept = 0;

  // Both overloads return the same host-owned service throughout this strategy
  // instance's lifetime. Borrow the reference only during the current callback;
  // the const overload permits queries without subscription changes.
  virtual MarketData& market_data() noexcept = 0;
  virtual const MarketData& market_data() const noexcept = 0;
  // Same borrowing and ownership rules as market_data(). Disabled trading
  // remains queryable, with requests rejected explicitly.
  virtual Trading& trading() noexcept = 0;
  virtual const Trading& trading() const noexcept = 0;
  // Same borrowing rules; one replaceable one-shot timer belongs to the
  // strategy instance. The const overload only inspects the armed deadline.
  virtual Timer& timer() noexcept = 0;
  virtual const Timer& timer() const noexcept = 0;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_STRATEGY_CONTEXT_H_
