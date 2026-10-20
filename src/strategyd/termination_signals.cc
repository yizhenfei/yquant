#include "strategyd/termination_signals.h"

#include <pthread.h>
#include <sys/select.h>

#include <cerrno>
#include <system_error>

namespace yquant::strategyd {
namespace {

volatile sig_atomic_t stop_signal = 0;

// This handler is allowed to run only in a main-thread signal check or wait.
// It must not format output, touch runtime objects, or perform cleanup.
void RecordStop(int signal) noexcept { stop_signal = signal; }

// pthread functions return their error number; errno-based calls pass errno.
base::Status SignalError(int error) noexcept {
  return {base::ErrorCode::kIoError, {error, std::generic_category()}};
}

}  // namespace

base::Status TerminationSignals::Prepare() noexcept {
  if (prepared_) {
    return {base::ErrorCode::kInvalidState, {}};
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

base::Status TerminationSignals::CheckStop(bool& out) noexcept {
  if (!prepared_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  const int unmask_error = pthread_sigmask(SIG_SETMASK, &wait_mask_, nullptr);
  // Always attempt restoration, including on an unmask failure. No strategy
  // or SDK operation is executed while these signals are unblocked.
  const int restore_error =
      pthread_sigmask(SIG_SETMASK, &blocked_mask_, nullptr);
  if (unmask_error != 0 || restore_error != 0) {
    return SignalError(unmask_error != 0 ? unmask_error : restore_error);
  }
  out = stop_signal != 0;
  return {};
}

base::Status TerminationSignals::Wait(
    std::optional<std::uint64_t> timeout_ns) noexcept {
  if (!prepared_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  if (timeout_ns && *timeout_ns == 0) {
    bool ignored = false;
    return CheckStop(ignored);
  }
  struct timespec timeout{};
  if (timeout_ns) {
    timeout.tv_sec = static_cast<time_t>(*timeout_ns / 1000000000);
    timeout.tv_nsec = static_cast<long>(*timeout_ns % 1000000000);
  }
  const int result = pselect(0, nullptr, nullptr, nullptr,
                             timeout_ns ? &timeout : nullptr, &wait_mask_);
  if (result < 0 && errno != EINTR) {
    return SignalError(errno);
  }
  return {};
}

}  // namespace yquant::strategyd
