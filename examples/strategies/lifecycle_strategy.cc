#include <cstdint>
#include <new>

#include "yquant/base/status.h"
#include "yquant/strategy/strategy.h"
#include "yquant/strategy/strategy_plugin.h"

namespace {

using yquant::base::ErrorCode;
using yquant::strategy::Strategy;
using yquant::strategy::StrategyPlugin;

class LifecycleStrategy final : public Strategy {
 public:
  ErrorCode OnStart() noexcept override {
    if (state_ != State::kIdle) {
      return ErrorCode::kInvalidState;
    }
    state_ = State::kRunning;
    return ErrorCode::kOk;
  }

  ErrorCode OnStop() noexcept override {
    if (state_ != State::kRunning) {
      return ErrorCode::kInvalidState;
    }
    state_ = State::kStopped;
    return ErrorCode::kOk;
  }

 private:
  enum class State { kIdle, kRunning, kStopped };

  State state_ = State::kIdle;
};

ErrorCode CreateStrategy(Strategy** output) noexcept {
  if (output == nullptr) {
    return ErrorCode::kInvalidArgument;
  }
  *output = nullptr;
  auto* strategy = new (std::nothrow) LifecycleStrategy;
  if (strategy == nullptr) {
    return ErrorCode::kResourceExhausted;
  }
  *output = strategy;
  return ErrorCode::kOk;
}

void DestroyStrategy(Strategy* strategy) noexcept { delete strategy; }

const StrategyPlugin kPlugin = {
    yquant::strategy::kStrategyPluginAbiVersion,
    static_cast<std::uint32_t>(sizeof(StrategyPlugin)), CreateStrategy,
    DestroyStrategy};

}  // namespace

extern "C" YQUANT_STRATEGY_EXPORT const StrategyPlugin*
YQuantGetStrategyPlugin() noexcept {
  return &kPlugin;
}
