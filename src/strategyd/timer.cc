#include "strategyd/timer.h"

namespace yquant::strategyd {

bool TimerImpl::CanChange() const noexcept {
  if (stopping_) return false;
  return phase_ == strategy::CallbackPhase::kStart ||
         (running_ && (phase_ == strategy::CallbackPhase::kTick ||
                       phase_ == strategy::CallbackPhase::kOrderUpdate ||
                       phase_ == strategy::CallbackPhase::kFill ||
                       phase_ == strategy::CallbackPhase::kTimer));
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
  const strategy::TimerEvent event{*deadline_, now};
  deadline_.reset();
  out = event;
  return true;
}

}  // namespace yquant::strategyd
