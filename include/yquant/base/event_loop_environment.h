#ifndef YQUANT_BASE_EVENT_LOOP_ENVIRONMENT_H_
#define YQUANT_BASE_EVENT_LOOP_ENVIRONMENT_H_

#include "yquant/base/status.h"

namespace yquant::base {

// Main-thread source of stop requests. Daemon loops check requests at startup
// and batch boundaries, and own waiting, event dispatch, and cleanup.
// TerminationSignals receives OS signals; tests supply controlled stop requests
// through the same interface.
class EventLoopEnvironment {
 public:
  virtual ~EventLoopEnvironment() noexcept = default;

  // Receives pending stop requests without waiting. On failure out is
  // unchanged.
  [[nodiscard]] virtual Status CheckStop(bool& out) noexcept = 0;
};

}  // namespace yquant::base

#endif  // YQUANT_BASE_EVENT_LOOP_ENVIRONMENT_H_
