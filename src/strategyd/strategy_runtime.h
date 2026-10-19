#ifndef YQUANT_STRATEGYD_STRATEGY_RUNTIME_H_
#define YQUANT_STRATEGYD_STRATEGY_RUNTIME_H_

#include <cstddef>
#include <string_view>

#include "strategyd/strategy_library.h"
#include "yquant/base/status.h"
#include "yquant/strategy/strategy.h"

namespace yquant::strategyd {

enum class RuntimeState { kIdle, kStarting, kRunning, kStopping, kFinished };

struct LoadAndStart {
  const char* library_path;
  const char* config_path;
};

struct Stop {};

// Commands and lifecycle callbacks execute serially on the owning thread.
class StrategyRuntime {
 public:
  StrategyRuntime() noexcept = default;
  ~StrategyRuntime() noexcept;

  StrategyRuntime(const StrategyRuntime&) = delete;
  StrategyRuntime& operator=(const StrategyRuntime&) = delete;
  StrategyRuntime(StrategyRuntime&&) = delete;
  StrategyRuntime& operator=(StrategyRuntime&&) = delete;

  [[nodiscard]] base::Status Handle(LoadAndStart command) noexcept;
  [[nodiscard]] base::Status Handle(Stop command) noexcept;

  [[nodiscard]] RuntimeState state() const noexcept { return state_; }
  // Owns a copy so diagnostics can identify a strategy after destruction.
  [[nodiscard]] std::string_view strategy_id() const noexcept {
    return strategy_id_.view();
  }
  // Diagnostics remain valid until the next command. Cleanup keeps the first
  // failure encountered by the current command.
  [[nodiscard]] const char* error_stage() const noexcept {
    return error_stage_;
  }
  [[nodiscard]] const char* error_detail() const noexcept {
    return error_detail_;
  }

 private:
  void ResetError() noexcept;
  void SetError(base::Status status, const char* stage,
                const char* detail) noexcept;
  void Cleanup() noexcept;

  static constexpr std::size_t kErrorDetailSize = 1024;
  StrategyLibrary library_;
  strategy::StrategyId strategy_id_;
  strategy::Strategy* instance_ = nullptr;
  RuntimeState state_ = RuntimeState::kIdle;
  bool started_ = false;
  base::Status result_;
  const char* error_stage_ = "";
  char error_detail_[kErrorDetailSize] = {};
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_STRATEGY_RUNTIME_H_
