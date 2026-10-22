#ifndef YQUANT_BASE_TERMINATION_SIGNALS_H_
#define YQUANT_BASE_TERMINATION_SIGNALS_H_

#include <signal.h>

#include <cstdint>
#include <optional>

#include "yquant/base/event_loop_environment.h"

namespace yquant::base {

// Process-lifetime signal ownership, used only by the main thread. Installs a
// handler that writes sig_atomic_t state, and creates no thread or descriptor.
// Prepare before loading any user code so threads it creates inherit blocking.
// Dispositions and the blocked mask intentionally remain installed until exit.
class TerminationSignals final : public EventLoopEnvironment {
 public:
  // Acquires SIGINT/SIGTERM once. On failure leaves them blocked if the initial
  // blocking succeeded; the caller should report the failure and exit.
  [[nodiscard]] Status Prepare() noexcept;

  // Briefly unmasks only the exit signals, then reblocks before returning.
  // This explicit window delivers pending signals even on platforms where a
  // zero-timeout pselect does not. Failure preserves out.
  [[nodiscard]] Status CheckStop(bool& out) noexcept override;

  // Waits with SIGINT/SIGTERM temporarily unblocked, restoring the calling
  // thread's mask on return. Requires successful Prepare. Signal interruption
  // succeeds; other wait errors are returned. Preserves unrelated signal bits
  // from the calling mask while atomically entering the wait.
  [[nodiscard]] Status Wait(
      std::optional<std::uint64_t> timeout_ns) noexcept override;

 private:
  sigset_t blocked_mask_ = {};
  sigset_t wait_mask_ = {};
  bool prepared_ = false;
};

}  // namespace yquant::base

#endif  // YQUANT_BASE_TERMINATION_SIGNALS_H_
