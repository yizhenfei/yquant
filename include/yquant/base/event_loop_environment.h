#ifndef YQUANT_BASE_EVENT_LOOP_ENVIRONMENT_H_
#define YQUANT_BASE_EVENT_LOOP_ENVIRONMENT_H_

#include <cstdint>
#include <optional>

#include "yquant/base/status.h"

namespace yquant::base {

// Main-thread services for waiting and receiving stop requests. Production
// uses OS signals; tests supply deterministic stop requests and wait results.
class EventLoopEnvironment {
 public:
  virtual ~EventLoopEnvironment() noexcept = default;

  // Receives pending stop requests without waiting. On failure out is
  // unchanged.
  [[nodiscard]] virtual Status CheckStop(bool& out) noexcept = 0;

  // Waits for at most timeout_ns, or indefinitely when absent. Interrupted
  // waits are successful; the caller must CheckStop and recompute its deadline.
  // Does not dispatch application work or retain arguments.
  [[nodiscard]] virtual Status Wait(
      std::optional<std::uint64_t> timeout_ns) noexcept = 0;
};

}  // namespace yquant::base

#endif  // YQUANT_BASE_EVENT_LOOP_ENVIRONMENT_H_
