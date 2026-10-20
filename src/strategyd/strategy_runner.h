#ifndef YQUANT_STRATEGYD_STRATEGY_RUNNER_H_
#define YQUANT_STRATEGYD_STRATEGY_RUNNER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "strategyd/event_loop_environment.h"
#include "strategyd/market_data.h"
#include "strategyd/mock_market_data_source.h"
#include "strategyd/strategy_context.h"
#include "strategyd/strategy_library.h"
#include "yquant/base/status.h"
#include "yquant/strategy/strategy.h"

namespace yquant::strategyd {

inline constexpr std::size_t kMaxMarketDataBatchSize = 64;

enum class RunnerState { kIdle, kStarting, kRunning, kStopping, kFinished };

struct LoadAndStart {
  const char* library_path;
  const char* config_path;
  const char* market_data_config_path = nullptr;
};

struct Stop {};

// Commands and lifecycle callbacks execute serially on the owning thread.
class StrategyRunner {
 public:
  // Optional dependencies are borrowed until destruction. Defaults use the
  // host's steady clock and synchronous mock source; environment is optional
  // for startup cancellation, but Run() requires an explicit environment.
  explicit StrategyRunner(const MonotonicClock* clock = nullptr,
                          MarketDataSource* source = nullptr,
                          EventLoopEnvironment* environment = nullptr) noexcept;
  ~StrategyRunner() noexcept;

  StrategyRunner(const StrategyRunner&) = delete;
  StrategyRunner& operator=(const StrategyRunner&) = delete;
  StrategyRunner(StrategyRunner&&) = delete;
  StrategyRunner& operator=(StrategyRunner&&) = delete;

  [[nodiscard]] base::Status Handle(LoadAndStart command) noexcept;
  [[nodiscard]] base::Status Handle(Stop command) noexcept;

  // Drives source polling and quote dispatch on the owning thread, waiting and
  // checking termination at batch boundaries. Requires Running, outside another
  // callback. On stop or failure, completes cleanup before returning; an
  // invalid state returns kInvalidState without changing resources.
  [[nodiscard]] base::Status Run(EventLoopEnvironment& environment) noexcept;
  std::optional<std::uint64_t> next_deadline() const noexcept;
  // Saturating total of structurally valid old/duplicate quotes ignored during
  // this run. Retained after Stop so host diagnostics can report it.
  std::uint64_t ignored_quote_count() const noexcept {
    return ignored_quote_count_;
  }

  [[nodiscard]] RunnerState state() const noexcept { return state_; }
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
  // Applies one source quote and invokes OnTick after updating the snapshot.
  // Requires Running, outside another callback. Valid old sequences are
  // ignored. Invalid data or callback failure stops and destroys the strategy,
  // preserving a copied diagnostic for this operation.
  [[nodiscard]] base::Status DispatchQuote(
      const model::QuoteTick& tick) noexcept;
  // Processes at most 64 source events (including ignored old records), without
  // waiting. Requires Running, outside another callback. Source or dispatch
  // failure completes cleanup and preserves the first operation diagnostic.
  [[nodiscard]] base::Status ProcessBatch() noexcept;
  // Starts an explicit host operation without changing its resources/counters.
  void ResetError() noexcept;
  // Copies only the first failure within an operation, including cleanup.
  void SetError(base::Status status, const char* stage,
                const char* detail) noexcept;
  // Copies field diagnostics before a loader or plugin buffer is released.
  void SetConfigError(base::Status status, const char* stage,
                      const strategy::ConfigError& error) noexcept;
  // Copies the quote's stable identity into the current operation diagnostic.
  void SetQuoteError(base::Status status, const char* stage, const char* detail,
                     const model::QuoteTick& tick) noexcept;
  // Returns true after cancellation/error has completed startup cleanup.
  bool CancelStartupIfRequested() noexcept;
  // Stops the source before callbacks and destruction, retaining operation
  // errors. Finished resources are never touched a second time.
  void Cleanup() noexcept;

  static constexpr std::size_t kErrorDetailSize = 1024;
  SteadyMonotonicClock default_clock_;
  const MonotonicClock& clock_;
  // Own the service before constructing the context that borrows it.
  MarketDataImpl market_data_;
  StrategyContextImpl context_;
  MockMarketDataSource default_source_;
  MarketDataSource& source_;
  EventLoopEnvironment* startup_environment_;
  StrategyLibrary library_;
  strategy::StrategyId strategy_id_;
  strategy::Strategy* instance_ = nullptr;
  RunnerState state_ = RunnerState::kIdle;
  bool started_ = false;
  bool source_prepared_ = false;
  bool source_started_ = false;
  bool callback_active_ = false;
  std::uint64_t ignored_quote_count_ = 0;
  base::Status result_;
  const char* error_stage_ = "";
  char error_detail_[kErrorDetailSize] = {};
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_STRATEGY_RUNNER_H_
