#ifndef YQUANT_TESTS_STRATEGYD_RUNNER_TEST_HARNESS_H_
#define YQUANT_TESTS_STRATEGYD_RUNNER_TEST_HARNESS_H_

#include <memory>

#include "strategyd/strategy_assembly.h"
#include "yquant/trading/mock_trading_adapter.h"

namespace yquant::strategyd {

// A real test source with no scheduled events. It still participates in
// Prepare/Start/Stop and gives lifecycle fixtures a configured dictionary.
class IdleTestSource final : public MarketDataSource {
 public:
  base::Status Prepare(const md::MarketDataConfig&) noexcept override {
    return {};
  }
  base::Status Start(std::span<const model::InstrumentId>,
                     std::uint64_t) noexcept override {
    return {};
  }
  MarketDataPollResult Poll(std::uint64_t,
                            model::QuoteTick&) noexcept override {
    return {};
  }
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    return {};
  }
  base::Status Stop() noexcept override { return {}; }
  const char* error_detail() const noexcept override { return ""; }
};

class IdleTestEnvironment final : public base::EventLoopEnvironment {
 public:
  base::Status CheckStop(bool& out) noexcept override {
    out = false;
    return {};
  }
  base::Status Wait(std::optional<std::uint64_t>) noexcept override {
    return {base::ErrorCode::kInternalError, {}};
  }
};

// Supplies explicit component lifetimes and file loading for runner fixtures.
// Production uses the same assembly but always takes its SHM receiver.
class RunnerTestHarness {
 public:
  RunnerTestHarness() noexcept
      : clock_(default_clock_),
        environment_(default_environment_),
        source_(idle_source_),
        assembly_(clock_, default_adapter_) {}
  RunnerTestHarness(base::EventLoopEnvironment& environment,
                    MarketDataSource& source) noexcept
      : clock_(default_clock_),
        environment_(environment),
        source_(source),
        assembly_(clock_, default_adapter_) {}
  RunnerTestHarness(const base::MonotonicClock& clock,
                    base::EventLoopEnvironment& environment,
                    MarketDataSource& source) noexcept
      : clock_(clock),
        environment_(environment),
        source_(source),
        assembly_(clock_, default_adapter_) {}
  RunnerTestHarness(const base::MonotonicClock& clock,
                    base::EventLoopEnvironment& environment,
                    MarketDataSource& source,
                    trading::TradingAdapter& adapter) noexcept
      : clock_(clock),
        environment_(environment),
        source_(source),
        assembly_(clock_, adapter) {}

  base::Status Handle(StrategyOptions options) noexcept {
    if (runner_ != nullptr) return runner_->Handle(assembly_.start_command());
    if (finished_) return {base::ErrorCode::kInvalidState, {}};
    if (options.market_data_config_path == nullptr)
      options.market_data_config_path = YQUANT_TEST_MARKET_DATA_CONFIG_PATH;
    const auto loaded = assembly_.Load(options, environment_);
    if (!loaded.ok() || assembly_.cancelled()) {
      finished_ = true;
      return loaded;
    }
    runner_ = std::make_unique<StrategyRunner>(clock_, environment_, source_,
                                               assembly_.trading());
    return runner_->Handle(assembly_.start_command());
  }
  base::Status Handle(Stop command) noexcept {
    if (runner_ != nullptr) return runner_->Handle(command);
    finished_ = true;
    return {};
  }
  base::Status Run() noexcept {
    return runner_ != nullptr
               ? runner_->Run()
               : base::Status{base::ErrorCode::kInvalidState, {}};
  }
  RunnerState state() const noexcept {
    return runner_ != nullptr ? runner_->state()
           : finished_        ? RunnerState::kFinished
                              : RunnerState::kIdle;
  }
  std::string_view strategy_id() const noexcept {
    return runner_ != nullptr ? runner_->strategy_id()
                              : assembly_.strategy_id();
  }
  const char* error_stage() const noexcept {
    return runner_ != nullptr ? runner_->error_stage()
                              : assembly_.error_stage();
  }
  const char* error_detail() const noexcept {
    return runner_ != nullptr ? runner_->error_detail()
                              : assembly_.error_detail();
  }
  std::optional<std::uint64_t> next_deadline() const noexcept {
    return runner_ != nullptr ? runner_->next_deadline() : std::nullopt;
  }
  std::uint64_t ignored_quote_count() const noexcept {
    return runner_ != nullptr ? runner_->ignored_quote_count() : 0;
  }

 private:
  base::SteadyMonotonicClock default_clock_;
  IdleTestEnvironment default_environment_;
  IdleTestSource idle_source_;
  trading::MockTradingAdapter default_adapter_;
  const base::MonotonicClock& clock_;
  base::EventLoopEnvironment& environment_;
  MarketDataSource& source_;
  StrategyAssembly assembly_;
  std::unique_ptr<StrategyRunner> runner_;
  bool finished_ = false;
};

}  // namespace yquant::strategyd
#endif  // YQUANT_TESTS_STRATEGYD_RUNNER_TEST_HARNESS_H_
