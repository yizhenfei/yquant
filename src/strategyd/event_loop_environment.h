#ifndef YQUANT_STRATEGYD_EVENT_LOOP_ENVIRONMENT_H_
#define YQUANT_STRATEGYD_EVENT_LOOP_ENVIRONMENT_H_

#include <cstdint>
#include <optional>

#include "yquant/base/status.h"

namespace yquant::strategyd {

// Main-thread services for waiting and receiving stop requests. During startup
// StrategyRunner optionally uses CheckStop() to cancel between stages.
// Run() requires an environment, checks for stop requests
// at loop/batch boundaries, and passes Wait() a timeout derived from the earliest
// source, timer, or shutdown deadline. The runner owns event dispatch and cleanup.
// TerminationSignals implements this interface with OS signals and pselect;
// tests substitute controlled stop requests and waits without changing the
// runner's scheduling logic.
class EventLoopEnvironment {
 public:
  virtual ~EventLoopEnvironment() noexcept = default;

  // Receives pending stop requests without waiting. On failure out is
  // unchanged.
  [[nodiscard]] virtual base::Status CheckStop(bool& out) noexcept = 0;

  // Waits for at most timeout_ns, or indefinitely when absent. Interrupted
  // waits are successful; the caller must CheckStop and recompute its deadline.
  // Does not call strategies, advance source state, or retain arguments.
  [[nodiscard]] virtual base::Status Wait(
      std::optional<std::uint64_t> timeout_ns) noexcept = 0;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_EVENT_LOOP_ENVIRONMENT_H_
