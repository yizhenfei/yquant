#ifndef YQUANT_STRATEGY_TIMER_H_
#define YQUANT_STRATEGY_TIMER_H_

#include <stdint.h>

#include <optional>

#include "yquant/base/status.h"

namespace yquant::strategy {

// One-shot notification in the same monotonic time domain as Context::Now().
// fire_time_ns is the host dispatch time and can be later than deadline_ns.
struct TimerEvent {
  uint64_t deadline_ns = 0;
  uint64_t fire_time_ns = 0;
};

// Host-owned single replaceable timer. Borrow only during the current callback
// on the strategy thread. No method invokes a callback synchronously.
class Timer {
 public:
  virtual ~Timer() noexcept = default;

  // Replaces the deadline, in the Context::Now() domain. Zero/past deadlines
  // request dispatch as soon as the main loop next selects this timer. Allowed
  // in OnStart and normal running business callbacks, including OnTimer.
  // Other phases return kInvalidState and preserve the previous deadline.
  [[nodiscard]] virtual base::ErrorCode Arm(uint64_t deadline_ns) noexcept = 0;

  // Same phase rules as Arm. Canceling an unarmed timer succeeds; failure
  // preserves state. Entering stopping automatically disarms the timer.
  [[nodiscard]] virtual base::ErrorCode Cancel() noexcept = 0;

  // Returns a copy; empty means unarmed. The host disarms before OnTimer, which
  // can Arm a new one-shot notification without recursive callback dispatch.
  virtual std::optional<uint64_t> deadline() const noexcept = 0;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_TIMER_H_
