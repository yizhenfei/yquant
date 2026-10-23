#ifndef YQUANT_BASE_EVENT_LOOP_ENVIRONMENT_H_
#define YQUANT_BASE_EVENT_LOOP_ENVIRONMENT_H_

#include <stdint.h>

#include <optional>

#include "yquant/base/status.h"

namespace yquant::base {

// Supplies stop checks and waiting for a main-thread event loop.
// Implementations coordinate waiting with their stop source; callers decide
// when to check, dispatch events, and clean up. Serialize all operations on the
// owning thread.
class EventLoopEnvironment {
 public:
  virtual ~EventLoopEnvironment() noexcept = default;

  // Receives pending stop requests without waiting. On failure out is
  // unchanged.
  [[nodiscard]] virtual Status CheckStop(bool& out) noexcept = 0;

  // Waits for a relative nanosecond timeout; nullopt permits indefinite waiting
  // until an implementation-specific wakeup. May return early, including for
  // stop activity. Success requires the caller to check stop requests again;
  // it does not itself mean that stopping was requested. Zero permits an
  // immediate return. Failures are reported without invoking loop callbacks.
  [[nodiscard]] virtual Status Wait(
      std::optional<uint64_t> timeout_ns) noexcept = 0;
};

}  // namespace yquant::base

#endif  // YQUANT_BASE_EVENT_LOOP_ENVIRONMENT_H_
