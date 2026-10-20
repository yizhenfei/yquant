#ifndef YQUANT_STRATEGYD_TERMINATION_SIGNALS_H_
#define YQUANT_STRATEGYD_TERMINATION_SIGNALS_H_

#include <signal.h>

#include "strategyd/event_loop_environment.h"

namespace yquant::strategyd {

// Process-lifetime signal ownership, used only by the main thread. Installs a
// handler that writes sig_atomic_t state, and creates no thread or descriptor.
// Prepare before loading any user code so threads it creates inherit blocking.
// Dispositions and the blocked mask intentionally remain installed until exit.
class TerminationSignals final : public EventLoopEnvironment {
 public:
  // Acquires SIGINT/SIGTERM once. On failure leaves them blocked if the initial
  // blocking succeeded; the caller should report the failure and exit.
  [[nodiscard]] base::Status Prepare() noexcept;

  // Briefly unmasks only the exit signals, then reblocks before returning.
  // This explicit window delivers pending signals even on platforms where a
  // zero-timeout pselect does not. Failure preserves out.
  [[nodiscard]] base::Status CheckStop(bool& out) noexcept override;

  // Only the wait temporarily unmasks exit signals. On return ordinary code
  // is blocked again; the event loop checks pending signals before dispatch.
  [[nodiscard]] base::Status Wait(
      std::optional<std::uint64_t> timeout_ns) noexcept override;

 private:
  sigset_t blocked_mask_ = {};
  sigset_t wait_mask_ = {};
  bool prepared_ = false;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_TERMINATION_SIGNALS_H_
