#ifndef YQUANT_STRATEGY_STRATEGY_H_
#define YQUANT_STRATEGY_STRATEGY_H_

#include "yquant/base/status.h"
#include "yquant/model/quote_tick.h"
#include "yquant/strategy/strategy_config.h"
#include "yquant/strategy/strategy_context.h"

namespace yquant::strategy {

// Callbacks execute serially on the host thread and must return promptly.
class Strategy {
 public:
  virtual ~Strategy() noexcept = default;

  // Always return the same owned configuration object for this instance.
  virtual StrategyConfig& config() noexcept = 0;

  // Receives fully initialized configuration and may register subscriptions.
  // On failure, roll back startup resources and remain safe to destroy. The
  // host then destroys the instance without calling OnStop.
  [[nodiscard]] virtual base::ErrorCode OnStart(
      StrategyContext& context) noexcept = 0;

  // Called after the host updates the latest snapshot. Both context and tick
  // are borrowed only for this call. A non-kOk result stops further dispatch
  // and begins error cleanup, including OnStop for a successfully started
  // instance. Lifecycle-only strategies may keep this no-op implementation.
  [[nodiscard]] virtual base::ErrorCode OnTick(
      StrategyContext&, const model::QuoteTick&) noexcept {
    return base::ErrorCode::kOk;
  }

  // Called once after successful OnStart, even if source activation fails.
  // No more quotes are dispatched; context.market_data() can still query the
  // last snapshot.
  // Stop all owned activity before returning, including when reporting an
  // error. The host subsequently destroys the instance and unloads its code.
  [[nodiscard]] virtual base::ErrorCode OnStop(
      StrategyContext& context) noexcept = 0;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_STRATEGY_H_
