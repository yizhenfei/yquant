#include "strategyd/timer.h"

namespace yquant::strategyd {

bool TimerImpl::CanChange() const noexcept {
  if (stopping_) return false;
  switch (phase_) {
    case strategy::CallbackPhase::kNone:
    case strategy::CallbackPhase::kStop:
      return false;
    case strategy::CallbackPhase::kStart:
      return true;
    case strategy::CallbackPhase::kTick:
    case strategy::CallbackPhase::kOrderUpdate:
    case strategy::CallbackPhase::kFill:
    case strategy::CallbackPhase::kTimer:
      return running_;
  }
  return false;
}

base::ErrorCode TimerImpl::Arm(std::uint64_t deadline_ns) noexcept {
  if (!CanChange()) return base::ErrorCode::kInvalidState;
  deadline_ = deadline_ns;
  return base::ErrorCode::kOk;
}

base::ErrorCode TimerImpl::Cancel() noexcept {
  if (!CanChange()) return base::ErrorCode::kInvalidState;
  deadline_.reset();
  return base::ErrorCode::kOk;
}

void TimerImpl::BeginStopping() noexcept {
  running_ = false;
  stopping_ = true;
  deadline_.reset();
}

void TimerImpl::Reset() noexcept {
  deadline_.reset();
  running_ = false;
  stopping_ = false;
  phase_ = strategy::CallbackPhase::kNone;
}

bool TimerImpl::TryTakeDue(std::uint64_t now,
                           strategy::TimerEvent& out) noexcept {
  if (!running_ || stopping_ || phase_ != strategy::CallbackPhase::kNone ||
      !deadline_ || *deadline_ > now) {
    return false;
  }
  // Consume before dispatch so a callback can rearm without the old one-shot
  // overwriting its replacement deadline.
  const strategy::TimerEvent event{*deadline_, now};
  deadline_.reset();
  out = event;
  return true;
}

}  // namespace yquant::strategyd
