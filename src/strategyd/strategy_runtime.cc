#include "strategyd/strategy_runtime.h"

#include <cstdio>

namespace yquant::strategyd {

StrategyRuntime::~StrategyRuntime() noexcept {
  // Explicit Stop reports errors; this is the ownership safety net.
  Cleanup();
}

void StrategyRuntime::ResetError() noexcept {
  result_ = {};
  error_stage_ = "";
  error_detail_[0] = '\0';
}

void StrategyRuntime::SetError(base::Status status, const char* stage,
                               const char* detail) noexcept {
  if (status.ok() || !result_.ok()) {
    return;
  }
  result_ = status;
  error_stage_ = stage;
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
}

base::Status StrategyRuntime::Handle(LoadAndStart command) noexcept {
  ResetError();
  if (state_ != RuntimeState::kIdle) {
    SetError({base::ErrorCode::kInvalidState, {}}, "command",
             "a strategy can only be loaded in the idle state");
    return result_;
  }

  state_ = RuntimeState::kStarting;
  const base::Status open_result = library_.Open(command.library_path);
  if (!open_result.ok()) {
    SetError(open_result, library_.error_stage(), library_.error_detail());
    Cleanup();
    return result_;
  }

  const auto* plugin = library_.plugin();
  const base::ErrorCode create_result = plugin->create(&instance_);
  if ((create_result == base::ErrorCode::kOk) != (instance_ != nullptr)) {
    SetError({base::ErrorCode::kCorrupt, {}}, "create",
             "strategy factory returned inconsistent status and instance");
  } else if (create_result != base::ErrorCode::kOk) {
    SetError({create_result, {}}, "create",
             "strategy factory returned an error");
  }
  if (!result_.ok()) {
    Cleanup();
    return result_;
  }

  const base::ErrorCode start_result = instance_->OnStart();
  if (start_result != base::ErrorCode::kOk) {
    SetError({start_result, {}}, "start", "strategy OnStart returned an error");
    Cleanup();
    return result_;
  }
  started_ = true;
  state_ = RuntimeState::kRunning;
  return result_;
}

base::Status StrategyRuntime::Handle(Stop) noexcept {
  ResetError();
  if (state_ == RuntimeState::kStarting || state_ == RuntimeState::kStopping) {
    SetError({base::ErrorCode::kInvalidState, {}}, "command",
             "a lifecycle callback is already in progress");
    return result_;
  }
  if (state_ != RuntimeState::kFinished) {
    Cleanup();
  }
  return result_;
}

void StrategyRuntime::Cleanup() noexcept {
  if (state_ == RuntimeState::kFinished) {
    return;
  }
  state_ = RuntimeState::kStopping;
  if (started_) {
    // Clear first so cleanup never calls OnStop more than once.
    started_ = false;
    const base::ErrorCode stop_result = instance_->OnStop();
    SetError({stop_result, {}}, "stop", "strategy OnStop returned an error");
  }
  if (instance_ != nullptr) {
    library_.plugin()->destroy(instance_);
    instance_ = nullptr;
  }
  const base::Status close_result = library_.Close();
  SetError(close_result, library_.error_stage(), library_.error_detail());
  state_ = RuntimeState::kFinished;
}

}  // namespace yquant::strategyd
