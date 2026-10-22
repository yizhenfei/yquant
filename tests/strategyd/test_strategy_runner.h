#ifndef YQUANT_TESTS_STRATEGYD_TEST_STRATEGY_RUNNER_H_
#define YQUANT_TESTS_STRATEGYD_TEST_STRATEGY_RUNNER_H_

// Test-only control of loop termination and simulated waiting. Production
// EventLoopEnvironment provides only stop checks; TestStrategyRunner overrides
// the runner's wait so fixtures can advance time without sleeping.

#include <cstdint>
#include <optional>

#include "strategyd/strategy_runner.h"

namespace yquant::strategyd {

// Shared test control used by RunnerTestHarness and its derived fixtures.
class TestLoopEnvironment : public base::EventLoopEnvironment {
 public:
  // Advances simulated time or injects a wait error. An absent timeout models
  // indefinite waiting, which fixtures must bound with a stop or failure.
  [[nodiscard]] virtual base::Status Wait(
      std::optional<std::uint64_t> timeout) noexcept = 0;
};

// Runs production scheduling with a fixture's controlled wait operation.
class TestStrategyRunner final : public StrategyRunner {
 public:
  // Borrows all dependencies until destruction, including the test wait.
  TestStrategyRunner(const base::MonotonicClock& clock,
                     TestLoopEnvironment& environment, MarketDataSource& source,
                     trading::TradingService& trading) noexcept
      : StrategyRunner(clock, environment, source, trading),
        environment_(environment) {}

 protected:
  base::Status Wait(std::optional<std::uint64_t> timeout) noexcept override {
    return environment_.Wait(timeout);
  }

 private:
  TestLoopEnvironment& environment_;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_TESTS_STRATEGYD_TEST_STRATEGY_RUNNER_H_
