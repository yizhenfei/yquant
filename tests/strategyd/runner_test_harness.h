#ifndef YQUANT_TESTS_STRATEGYD_RUNNER_TEST_HARNESS_H_
#define YQUANT_TESTS_STRATEGYD_RUNNER_TEST_HARNESS_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "mock_trading_service.h"
#include "strategyd/strategy_assembly.h"
#include "strategyd/strategy_runner.h"

namespace yquant::strategyd {

// A real test source with no scheduled events. It still participates in
// Prepare/Start/Stop and gives lifecycle fixtures a configured dictionary.
class IdleTestSource final : public MarketDataSource {
 public:
  base::Status Prepare(const md::MarketDataConfig&) noexcept override {
    return {};
  }
  base::Status Start(std::span<const model::InstrumentId>,
                     uint64_t) noexcept override {
    return {};
  }
  MarketDataPollResult Poll(uint64_t, model::QuoteTick&) noexcept override {
    return {};
  }
  std::optional<uint64_t> next_deadline() const noexcept override { return {}; }
  base::Status Stop() noexcept override { return {}; }
  const char* error_detail() const noexcept override { return ""; }
};

// Reports controlled stop requests; unexpected waits fail instead of blocking.
class IdleTestEnvironment final : public base::EventLoopEnvironment {
 public:
  // Makes subsequent CheckStop calls report a stop request.
  void RequestStop() noexcept { stop_requested_ = true; }
  base::Status CheckStop(bool& out) noexcept override {
    out = stop_requested_;
    return {};
  }
  base::Status Wait(std::optional<uint64_t>) noexcept override {
    return {base::ErrorCode::kInternalError, {}};
  }

 private:
  bool stop_requested_ = false;
};

// Supplies explicit component lifetimes and file loading for runner fixtures.
// Production uses the selected receive source and TCP trading client.
class RunnerTestHarness {
 public:
  RunnerTestHarness() noexcept
      : clock_(default_clock_),
        environment_(default_environment_),
        source_(idle_source_),
        service_(default_service_),
        assembly_(clock_) {}
  RunnerTestHarness(base::EventLoopEnvironment& environment,
                    MarketDataSource& source) noexcept
      : clock_(default_clock_),
        environment_(environment),
        source_(source),
        service_(default_service_),
        assembly_(clock_) {}
  RunnerTestHarness(const base::MonotonicClock& clock,
                    base::EventLoopEnvironment& environment,
                    MarketDataSource& source) noexcept
      : clock_(clock),
        environment_(environment),
        source_(source),
        service_(default_service_),
        assembly_(clock_) {}
  RunnerTestHarness(const base::MonotonicClock& clock,
                    base::EventLoopEnvironment& environment,
                    MarketDataSource& source,
                    trading::TradingService& service) noexcept
      : clock_(clock),
        environment_(environment),
        source_(source),
        service_(service),
        assembly_(clock_) {}

  base::Status LoadAndStart(StrategyOptions options) noexcept {
    if (runner_ != nullptr) {
      return runner_->LoadAndStart(assembly_.start_options());
    }
    if (finished_) {
      return {base::ErrorCode::kInvalidState, {}};
    }
    if (options.market_data_config_path == nullptr) {
      options.market_data_config_path = YQUANT_TEST_MARKET_DATA_CONFIG_PATH;
    }
    if (options.trading_config_path == nullptr) {
      options.trading_config_path = YQUANT_TEST_TRADING_CLIENT_CONFIG_PATH;
    }
    const auto loaded = assembly_.Load(options, environment_);
    if (!loaded.ok() || assembly_.cancelled()) {
      finished_ = true;
      return loaded;
    }
    runner_ = std::make_unique<StrategyRunner>(clock_, environment_, source_,
                                               service_);
    return runner_->LoadAndStart(assembly_.start_options());
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
  uint64_t ignored_quote_count() const noexcept {
    return runner_ != nullptr ? runner_->ignored_quote_count() : 0;
  }

 private:
  base::SteadyMonotonicClock default_clock_;
  IdleTestEnvironment default_environment_;
  IdleTestSource idle_source_;
  testing::MockTradingService default_service_;
  const base::MonotonicClock& clock_;
  base::EventLoopEnvironment& environment_;
  MarketDataSource& source_;
  trading::TradingService& service_;
  StrategyAssembly assembly_;
  std::unique_ptr<StrategyRunner> runner_;
  bool finished_ = false;
};

}  // namespace yquant::strategyd
#endif  // YQUANT_TESTS_STRATEGYD_RUNNER_TEST_HARNESS_H_
