#ifndef YQUANT_STRATEGY_STRATEGY_H_
#define YQUANT_STRATEGY_STRATEGY_H_

#include "yquant/base/status.h"
#include "yquant/model/quote_tick.h"
#include "yquant/model/trading_event.h"
#include "yquant/strategy/strategy_config.h"
#include "yquant/strategy/strategy_context.h"
#include "yquant/strategy/timer.h"

namespace yquant::strategy {

// Callbacks execute serially on the host thread and must return promptly.
class Strategy {
 public:
  virtual ~Strategy() noexcept = default;

  // Always return the same owned configuration object for this instance.
  virtual StrategyConfig& config() noexcept = 0;

  // Receives initialized configuration and may subscribe or arm the timer;
  // timer callbacks start only after every startup stage succeeds.
  // Trading requests are disabled until all startup stages succeed.
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

  // The host has committed the complete report before calling either callback.
  // A fill report calls OnOrderUpdate then OnFill without another broker event
  // between them. Local requests from the first callback are visible in the
  // second. Non-kOk stops business callbacks; committed facts are not rolled
  // back. During stopping these callbacks may query but cannot send requests.
  [[nodiscard]] virtual base::ErrorCode OnOrderUpdate(
      StrategyContext&, const model::OrderUpdate&) noexcept {
    return base::ErrorCode::kOk;
  }
  [[nodiscard]] virtual base::ErrorCode OnFill(StrategyContext&,
                                               const model::Fill&) noexcept {
    return base::ErrorCode::kOk;
  }

  // Called only after complete startup and while Running. The one-shot has
  // already been disarmed and may be rearmed for a later main-loop dispatch.
  // References are borrowed for this callback only. Failure suppresses further
  // business callbacks and begins order draining without reverting facts.
  [[nodiscard]] virtual base::ErrorCode OnTimer(StrategyContext&,
                                                const TimerEvent&) noexcept {
    return base::ErrorCode::kOk;
  }

  // Called once after successful OnStart, even if source activation fails.
  // The timer has been disarmed; timer/request mutation is disabled.
  // No more quotes are dispatched; context.market_data() can still query the
  // last snapshot.
  // Stop all owned activity before returning, including when reporting an
  // error. The host subsequently destroys the instance and unloads its code.
  [[nodiscard]] virtual base::ErrorCode OnStop(
      StrategyContext& context) noexcept = 0;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_STRATEGY_H_
