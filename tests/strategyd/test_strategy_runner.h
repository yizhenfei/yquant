#ifndef YQUANT_TESTS_STRATEGYD_TEST_STRATEGY_RUNNER_H_
#define YQUANT_TESTS_STRATEGYD_TEST_STRATEGY_RUNNER_H_

#include <cstdint>
#include <optional>

#include "strategyd/strategy_runner.h"

namespace yquant::strategyd {

// Runs the production loop with a test-controlled wait. Each environment's
// CheckStop supplies termination; its separate Wait advances simulated time or
// injects waiting failures. Neither operation sleeps in deterministic tests.
class TestStrategyRunner final : public StrategyRunner {
 public:
  using StrategyRunner::StrategyRunner;

  // Borrows environment only for this call, including its Wait operation.
  template <typename Environment>
  [[nodiscard]] base::Status Run(Environment& environment) noexcept {
    wait_context_ = &environment;
    wait_ = [](void* context, std::optional<std::uint64_t> timeout) noexcept {
      return static_cast<Environment*>(context)->Wait(timeout);
    };
    const auto result = StrategyRunner::Run(environment);
    wait_ = nullptr;
    wait_context_ = nullptr;
    return result;
  }

 protected:
  base::Status Wait(std::optional<std::uint64_t> timeout) noexcept override {
    return wait_(wait_context_, timeout);
  }

 private:
  // Both are set only while Run() borrows the concrete test environment.
  void* wait_context_ = nullptr;
  base::Status (*wait_)(void*, std::optional<std::uint64_t>) noexcept = nullptr;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_TESTS_STRATEGYD_TEST_STRATEGY_RUNNER_H_
