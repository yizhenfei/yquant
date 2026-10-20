#ifndef YQUANT_STRATEGYD_EVENT_LOOP_ENVIRONMENT_H_
#define YQUANT_STRATEGYD_EVENT_LOOP_ENVIRONMENT_H_

#include <cstdint>
#include <optional>

#include "yquant/base/status.h"

namespace yquant::strategyd {

// Main-thread services for waiting and receiving stop requests. Production
// uses OS signals; tests supply deterministic stop requests and wait results.
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
