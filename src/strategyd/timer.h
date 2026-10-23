#ifndef YQUANT_STRATEGYD_TIMER_H_
#define YQUANT_STRATEGYD_TIMER_H_

#include <stdint.h>

#include <optional>

#include "strategyd/market_data.h"
#include "yquant/strategy/timer.h"

namespace yquant::strategyd {

// Implements the strategy's replaceable one-shot timer. The runner enables
// dispatch after startup, consumes due notifications before OnTimer, and
// disarms on stopping. All access is serialized on the owning strategy thread;
// timer operations allocate no memory and never invoke callbacks themselves.
class TimerImpl final : public strategy::Timer {
 public:
  TimerImpl() = default;
  TimerImpl(const TimerImpl&) = delete;
  TimerImpl& operator=(const TimerImpl&) = delete;

  // Enables or suspends consumption without changing an armed deadline. The
  // runner enables it after startup; BeginStopping permanently closes mutation
  // and consumption until Reset.
  void SetRunning(bool running) noexcept { running_ = running; }
  // Sets the current strategy callback phase. Calls must not nest and must be
  // paired with EndCallback, including when a callback fails.
  void BeginCallback(strategy::CallbackPhase phase) noexcept { phase_ = phase; }
  // Leaves the current callback phase, disabling strategy timer mutation.
  void EndCallback() noexcept { phase_ = strategy::CallbackPhase::kNone; }
  // Disarms the timer and rejects further mutation or consumption until Reset.
  // Repeated calls preserve the stopping state.
  void BeginStopping() noexcept;
  // Restores an unarmed pre-start state. Call only outside strategy callbacks.
  void Reset() noexcept;

  [[nodiscard]] base::ErrorCode Arm(uint64_t deadline_ns) noexcept override;
  [[nodiscard]] base::ErrorCode Cancel() noexcept override;
  std::optional<uint64_t> deadline() const noexcept override {
    return deadline_;
  }

  // Consumes a due notification outside callbacks while running. Uses absolute
  // monotonic now; success disarms the timer and overwrites out with its
  // deadline and actual dispatch time. False preserves out and timer state.
  [[nodiscard]] bool TryTakeDue(uint64_t now,
                                strategy::TimerEvent& out) noexcept;

 private:
  // Returns whether the current lifecycle and callback phase allow mutation.
  bool CanChange() const noexcept;

  std::optional<uint64_t> deadline_;  // Absolute nanoseconds, or unarmed.
  bool running_ = false;
  bool stopping_ = false;
  strategy::CallbackPhase phase_ = strategy::CallbackPhase::kNone;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_TIMER_H_
