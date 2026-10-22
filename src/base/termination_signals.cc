#include "yquant/base/termination_signals.h"

#include <pthread.h>
#include <sys/select.h>

#include <cerrno>
#include <system_error>

namespace yquant::base {
namespace {

volatile sig_atomic_t stop_signal = 0;

// This handler is allowed to run only in a main-thread signal check or wait.
// It must not format output, touch runtime objects, or perform cleanup.
void RecordStop(int signal) noexcept { stop_signal = signal; }

// pthread functions return their error number; errno-based calls pass errno.
Status SignalError(int error) noexcept {
  return {ErrorCode::kIoError, {error, std::generic_category()}};
}

}  // namespace

Status TerminationSignals::Prepare() noexcept {
  if (prepared_) {
    return {ErrorCode::kInvalidState, {}};
  }
  sigset_t exits;
  if (sigemptyset(&exits) != 0 || sigaddset(&exits, SIGINT) != 0 ||
      sigaddset(&exits, SIGTERM) != 0) {
    return SignalError(errno);
  }
  sigset_t original;
  const int block_error = pthread_sigmask(SIG_BLOCK, &exits, &original);
  if (block_error != 0) {
    return SignalError(block_error);
  }
  blocked_mask_ = original;
  wait_mask_ = original;
  if (sigaddset(&blocked_mask_, SIGINT) != 0 ||
      sigaddset(&blocked_mask_, SIGTERM) != 0 ||
      sigdelset(&wait_mask_, SIGINT) != 0 ||
      sigdelset(&wait_mask_, SIGTERM) != 0) {
    return SignalError(errno);
  }
  stop_signal = 0;
  struct sigaction action{};
  action.sa_handler = &RecordStop;
  action.sa_mask = exits;
  if (sigaction(SIGINT, &action, nullptr) != 0 ||
      sigaction(SIGTERM, &action, nullptr) != 0) {
    return SignalError(errno);
  }
  prepared_ = true;
  return {};
}

Status TerminationSignals::CheckStop(bool& out) noexcept {
  if (!prepared_) {
    return {ErrorCode::kInvalidState, {}};
  }
  const int unmask_error = pthread_sigmask(SIG_SETMASK, &wait_mask_, nullptr);
  // Always attempt restoration, including on an unmask failure. No
  // application work is executed while these signals are unblocked.
  const int restore_error =
      pthread_sigmask(SIG_SETMASK, &blocked_mask_, nullptr);
  if (unmask_error != 0 || restore_error != 0) {
    return SignalError(unmask_error != 0 ? unmask_error : restore_error);
  }
  out = stop_signal != 0;
  return {};
}

Status TerminationSignals::Wait(
    std::optional<std::uint64_t> timeout_ns) noexcept {
  if (!prepared_) return {ErrorCode::kInvalidState, {}};
  sigset_t wait_mask;
  const int mask_error = pthread_sigmask(SIG_SETMASK, nullptr, &wait_mask);
  if (mask_error != 0) return SignalError(mask_error);
  // Unmask only termination signals. pselect changes the mask atomically with
  // sleeping, so a pending signal cannot be lost between a stop check and wait.
  if (sigdelset(&wait_mask, SIGINT) != 0 ||
      sigdelset(&wait_mask, SIGTERM) != 0) {
    return SignalError(errno);
  }
  struct timespec timeout{};
  if (timeout_ns) {
    // All uint64_t nanosecond durations fit in the supported 64-bit time_t
    // after conversion to seconds.
    constexpr std::uint64_t kNanosecondsPerSecond = 1000000000;
    timeout.tv_sec = static_cast<time_t>(*timeout_ns / kNanosecondsPerSecond);
    timeout.tv_nsec = static_cast<long>(*timeout_ns % kNanosecondsPerSecond);
  }
  const int result = pselect(0, nullptr, nullptr, nullptr,
                             timeout_ns ? &timeout : nullptr, &wait_mask);
  if (result < 0 && errno != EINTR) return SignalError(errno);
  return {};
}

}  // namespace yquant::base
