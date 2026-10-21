#ifndef YQUANT_STRATEGYD_TIMER_H_
#define YQUANT_STRATEGYD_TIMER_H_

#include <cstdint>
#include <optional>

#include "strategyd/market_data.h"
#include "yquant/strategy/timer.h"

namespace yquant::strategyd {

// Runner-owned, allocation-free one-shot state. Phase and lifecycle changes,
// mutation, and consumption all occur on the same owning strategy thread.
class TimerImpl final : public strategy::Timer {
 public:
  // Enables dispatch after all startup stages succeed; preserves a deadline
  // armed in OnStart. BeginStopping is the explicit closing/disarming boundary.
  void SetRunning(bool running) noexcept { running_ = running; }
  void BeginCallback(strategy::CallbackPhase phase) noexcept { phase_ = phase; }
  void EndCallback() noexcept { phase_ = strategy::CallbackPhase::kNone; }
  // Idempotently closes mutation and discards any pending notification.
  void BeginStopping() noexcept;
  // Restores empty startup state outside callbacks; owns no external resource.
  void Reset() noexcept;

  [[nodiscard]] base::ErrorCode Arm(
      std::uint64_t deadline_ns) noexcept override;
  [[nodiscard]] base::ErrorCode Cancel() noexcept override;
  std::optional<std::uint64_t> deadline() const noexcept override {
    return deadline_;
  }

  // Host-only consumption outside callbacks, enabled only while Running.
  // A due deadline is disarmed before writing out; false preserves both out
  // and state. The caller then invokes OnTimer without retaining any borrow.
  [[nodiscard]] bool TryTakeDue(std::uint64_t now,
                                strategy::TimerEvent& out) noexcept;

 private:
  bool CanChange() const noexcept;

  std::optional<std::uint64_t> deadline_;
  bool running_ = false;
  bool stopping_ = false;
  strategy::CallbackPhase phase_ = strategy::CallbackPhase::kNone;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_TIMER_H_
