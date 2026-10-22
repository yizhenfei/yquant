#ifndef YQUANT_STRATEGYD_EVENT_LOOP_ENVIRONMENT_H_
#define YQUANT_STRATEGYD_EVENT_LOOP_ENVIRONMENT_H_

#include "yquant/base/status.h"

namespace yquant::strategyd {

// Main-thread source of stop requests. During startup StrategyRunner optionally
// uses CheckStop() to cancel between stages.
// Run() checks requests at loop/batch boundaries and owns waiting, event
// dispatch, and cleanup. TerminationSignals receives OS signals; tests supply
// controlled stop requests through the same interface.
class EventLoopEnvironment {
 public:
  virtual ~EventLoopEnvironment() noexcept = default;

  // Receives pending stop requests without waiting. On failure out is
  // unchanged.
  [[nodiscard]] virtual base::Status CheckStop(bool& out) noexcept = 0;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_EVENT_LOOP_ENVIRONMENT_H_
