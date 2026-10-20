#ifndef YQUANT_STRATEGYD_STRATEGY_RUNNER_H_
#define YQUANT_STRATEGYD_STRATEGY_RUNNER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "strategyd/event_loop_environment.h"
#include "strategyd/market_data.h"
#include "strategyd/mock_market_data_source.h"
#include "strategyd/mock_trading_adapter.h"
#include "strategyd/shm_market_data_source.h"
#include "strategyd/strategy_context.h"
#include "strategyd/strategy_library.h"
#include "strategyd/trading.h"
#include "yquant/base/status.h"
#include "yquant/strategy/strategy.h"

namespace yquant::strategyd {

inline constexpr std::size_t kMaxStrategyBatchSize = 64;

enum class RunnerState { kIdle, kStarting, kRunning, kStopping, kFinished };

struct LoadAndStart {
  const char* library_path;
  const char* config_path;
  const char* market_data_config_path = nullptr;
  const char* trading_config_path = nullptr;
};

struct Stop {};

// Commands and lifecycle callbacks execute serially on the owning thread.
class StrategyRunner {
 public:
  // Optional dependencies are borrowed until destruction. Defaults use the
  // host's steady clock and a mock/SHM source selected by configuration.
  // An injected source overrides that selection. Environment is optional for
  // startup cancellation, but Run() requires an explicit environment.
  explicit StrategyRunner(const MonotonicClock* clock = nullptr,
                          MarketDataSource* source = nullptr,
                          EventLoopEnvironment* environment = nullptr,
                          TradingAdapter* trading_adapter = nullptr) noexcept;
  ~StrategyRunner() noexcept;

  StrategyRunner(const StrategyRunner&) = delete;
  StrategyRunner& operator=(const StrategyRunner&) = delete;
  StrategyRunner(StrategyRunner&&) = delete;
  StrategyRunner& operator=(StrategyRunner&&) = delete;

  [[nodiscard]] base::Status Handle(LoadAndStart command) noexcept;
  // Running requests are recorded for Run to drain, without invoking callbacks.
  // Idle/finished stops are immediate and idempotent. No callback reentry.
  [[nodiscard]] base::Status Handle(Stop command) noexcept;

  // Drives requests and both event sources on the owning thread. Requires
  // Running, outside callbacks. Stops close new requests and drain outstanding
  // orders before finalization. Failures retain the first diagnostic; invalid
  // state leaves resources unchanged. All dependencies outlive this call.
  [[nodiscard]] base::Status Run(EventLoopEnvironment& environment) noexcept;
  // Earliest source event, cold maintenance, local work, or drain deadline.
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
  // Applies one quote before its callback. False starts error draining; old
  // valid quotes succeed without a callback. Called only while Running.
  bool DispatchQuote(const model::QuoteTick& tick) noexcept;
  // Dispatches a committed trading event without reading another source between
  // its two callbacks. Failure disables subsequent business callbacks.
  bool DispatchTrading(const TradingDispatch& dispatch) noexcept;
  enum class QuotePollResult { kEvent, kIdle, kError };
  // Polls one market record. An idle result yields this source's remaining
  // batch budget, including a poll-driven source whose deadline stays due.
  QuotePollResult PollQuote() noexcept;
  // Polls one local/broker event; false starts error stopping. Broker Idle must
  // retain a future or absent deadline.
  bool PollTrading() noexcept;
  // Processes at most 64 polls/local events with persistent source rotation.
  // After market Idle, only trading may use this batch's remaining budget.
  void ProcessBatch() noexcept;
  // Establishes non-nested callback phases on both host services.
  void BeginCallback(CallbackPhase phase) noexcept;
  void EndCallback() noexcept;
  // Receives stop requests; a failed environment makes further draining unsafe.
  void CheckStop(EventLoopEnvironment& environment) noexcept;
  // Closes new strategy requests and stops market data once; a second call
  // preserves the original drain deadline and outstanding orders.
  void EnterStopping() noexcept;
  // Starts error stopping, optionally retaining a usable trading channel.
  void StopAfterError(bool can_drain) noexcept;
  bool IsDrained() const noexcept;
  // Completes stopping when drained, failed, or timed out. True means
  // finalized.
  bool FinishStoppingIfReady() noexcept;
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
  // Copies order identity before an event or plugin-owned buffer is released.
  void SetOrderError(base::Status status, const char* stage, const char* detail,
                     model::ClientOrderId id, std::uint64_t sequence) noexcept;
  // Returns true after cancellation/error has completed startup cleanup.
  bool CancelStartupIfRequested() noexcept;
  // Reports unconfirmed order facts before owned ledger storage is reset.
  void ReportUnconfirmed() const noexcept;
  // Final lifecycle cleanup, after draining or a startup/unrecoverable error.
  // Keeps the ledger alive through OnStop and destroys the plugin before reset.
  void Finalize() noexcept;

  static constexpr std::size_t kErrorDetailSize = 1024;
  SteadyMonotonicClock default_clock_;
  const MonotonicClock& clock_;
  // Own the service before constructing the context that borrows it.
  MarketDataImpl market_data_;
  TradingImpl trading_;
  StrategyContextImpl context_;
  MockMarketDataSource mock_source_;
  ShmMarketDataSource shm_source_;
  MarketDataSource* source_;
  MockTradingAdapter default_trading_adapter_;
  TradingAdapter& trading_adapter_;
  EventLoopEnvironment* startup_environment_;
  StrategyLibrary library_;
  strategy::StrategyId strategy_id_;
  strategy::Strategy* instance_ = nullptr;
  RunnerState state_ = RunnerState::kIdle;
  bool started_ = false;
  bool source_prepared_ = false;
  bool source_started_ = false;
  bool trading_prepared_ = false;
  bool trading_started_ = false;
  bool callback_active_ = false;
  bool stop_requested_ = false;
  bool callbacks_enabled_ = true;
  bool drain_viable_ = true;
  bool prefer_market_ = true;
  bool prefer_local_ = true;
  std::uint64_t shutdown_timeout_ns_ = 5000000000ULL;
  std::optional<std::uint64_t> shutdown_deadline_;
  std::uint64_t ignored_quote_count_ = 0;
  base::Status result_;
  const char* error_stage_ = "";
  char error_detail_[kErrorDetailSize] = {};
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_STRATEGY_RUNNER_H_
