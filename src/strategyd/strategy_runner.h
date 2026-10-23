#ifndef YQUANT_STRATEGYD_STRATEGY_RUNNER_H_
#define YQUANT_STRATEGYD_STRATEGY_RUNNER_H_

// Defines startup inputs and observable lifecycle state for the strategyd
// strategy runner. Market-data access, trading, and plugin loading are supplied
// by separate components; this interface coordinates their use by one strategy.

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string_view>

#include "strategyd/market_data.h"
#include "strategyd/market_data_source.h"
#include "strategyd/strategy_config_loader.h"
#include "strategyd/strategy_context.h"
#include "strategyd/strategy_library.h"
#include "strategyd/timer.h"
#include "yquant/base/event_loop_environment.h"
#include "yquant/base/status.h"
#include "yquant/strategy/strategy.h"
#include "yquant/trading/trading_service.h"

namespace yquant::strategyd {

// Maximum market polls, trading events, and timer dispatches per batch, shared
// across all sources to bound work between stop/deadline checks. Ignored and
// duplicate quotes also consume this budget.
inline constexpr size_t kMaxStrategyBatchSize = 64;

// Lifecycle of a single load attempt. Finished is terminal, including after
// startup failure; a new attempt requires a new runner.
enum class RunnerState { kIdle, kStarting, kRunning, kStopping, kFinished };

// Supplies assembly-validated inputs to LoadAndStart, compatible with the
// injected source and trading service. References are borrowed for that call;
// the configuration loader may be reset after plugin initialization.
struct StrategyStartOptions {
  // Null-terminated plugin path. Null and empty paths are invalid.
  const char* library_path;
  StrategyConfigLoader& config;
  const md::MarketDataConfig& market_data;
  const trading::TradingClientConfig& trading_config;
};

// Hosts one strategy instance and dispatches market, trading, and timer events
// to it. Owns the plugin instance, library handle, and callback context; uses
// injected components for market-data ingestion and trading.
//
// Call LoadAndStart once, then Run if it succeeds. Run drives events
// until the environment requests stopping, the source finishes, or an error
// occurs, then completes shutdown. Destruction releases remaining resources
// without running an order-draining loop.
//
// All calls and destruction must be serialized on the owning thread. Strategy
// callbacks run on that thread and must not reenter mutating runner operations.
class StrategyRunner final {
 public:
  // Borrows all four dependencies until destruction. Run uses environment to
  // check for stop requests; loading and startup do not inspect it.
  StrategyRunner(const base::MonotonicClock& clock,
                 base::EventLoopEnvironment& environment,
                 MarketDataSource& source,
                 trading::TradingService& trading) noexcept;
  // Calls OnStop if OnStart succeeded, destroys the instance, and releases
  // remaining resources. Does not wait for outstanding orders to drain; use
  // Run to complete orderly shutdown before destruction.
  ~StrategyRunner() noexcept;

  StrategyRunner(const StrategyRunner&) = delete;
  StrategyRunner& operator=(const StrategyRunner&) = delete;
  StrategyRunner(StrategyRunner&&) = delete;
  StrategyRunner& operator=(StrategyRunner&&) = delete;

  // Loads, configures, and starts the strategy from kIdle. Resets
  // `options.config` after attempting plugin configuration initialization, even
  // if that step fails; earlier failures leave the loader unchanged.
  //
  // An OK result always enters kRunning and permits Run. Does not inspect
  // environment or process stop requests; Run handles them before dispatch.
  // Startup failure cleans up and enters kFinished.
  // Calls OnStop during cleanup only if OnStart succeeded.
  // Outside kIdle, returns kInvalidState and replaces diagnostics, preserving
  // the existing lifecycle state and component resources.
  [[nodiscard]] base::Status LoadAndStart(
      StrategyStartOptions options) noexcept;

  // Drives requests and events until shutdown completes and enters kFinished.
  // Stops on an environment request, source completion, or an error. Closes new
  // strategy requests and drains outstanding trading work while the channel is
  // usable, bounded by the configured shutdown timeout, before final cleanup.
  // Returns the first error, including cleanup errors, or OK on normal exit.
  // Returns kInvalidState outside kRunning or during a callback, leaving state,
  // resources, and diagnostics unchanged.
  [[nodiscard]] base::Status Run() noexcept;

  // Returns the number of valid old or duplicate quotes ignored by this runner,
  // saturated at UINT64_MAX and retained after shutdown.
  uint64_t ignored_quote_count() const noexcept { return ignored_quote_count_; }

  [[nodiscard]] RunnerState state() const noexcept { return state_; }
  // Returns a view of the runner's copied ID, retained after plugin teardown.
  // Empty until startup copies the configuration ID; valid until destruction
  // of this runner.
  [[nodiscard]] std::string_view strategy_id() const noexcept {
    return strategy_id_.view();
  }
  // Returns the stage of the first failure in the current operation, or an
  // empty string. Cleanup preserves that failure. Diagnostic contents may be
  // replaced by subsequent LoadAndStart or Run calls.
  [[nodiscard]] const char* error_stage() const noexcept {
    return error_stage_;
  }
  // Returns the runner-owned, null-terminated detail for that failure, or an
  // empty string. May be truncated; contents follow error_stage's reset rules.
  [[nodiscard]] const char* error_detail() const noexcept {
    return error_detail_;
  }

 private:
  // Reports quote dispatch success and whether trading can drain on failure.
  struct QuoteDispatchResult {
    bool success;
    // Used only on failure; false means the trading service cannot drain.
    bool can_drain;
  };
  // Updates market and trading views before OnTick while kRunning. Borrows tick
  // only for this call. Valid old/duplicate quotes succeed without a callback.
  // Records validation, trading, or callback failures and returns the drain
  // policy; the caller must begin stopping on failure.
  [[nodiscard]] QuoteDispatchResult DispatchQuote(
      const model::QuoteTick& tick) noexcept;
  // Delivers a committed event's order update before its fill, when present,
  // without polling another source between callbacks. Borrows dispatch only
  // for this call. Returns true if callbacks succeed or are disabled; callback
  // failure disables further business callbacks, begins stopping, and returns
  // false. Committed trading facts are preserved.
  bool DispatchTrading(const trading::TradingDispatch& dispatch) noexcept;
  // Dispatches at most one due timer, disarming it before OnTimer so the
  // callback can rearm it. Requires kRunning and no active callback.
  // Returns true if no timer is due or the callback succeeds. On callback
  // failure, records the error, begins stopping, and returns false.
  bool DispatchTimer() noexcept;
  // Distinguishes an accepted or ignored quote from a source yield or failure.
  enum class QuotePollResult { kEvent, kIdle, kError };
  // Polls the active market source once and dispatches a returned quote.
  // Returns kIdle on a yield or completion, beginning stopping on completion.
  // Polling, deadline-contract, or dispatch failure returns kError and begins
  // stopping. Requires kRunning and a started source.
  QuotePollResult PollQuote() noexcept;
  // Polls and dispatches at most one trading event. Requires a started trading
  // service. Returns true on success, including no event; a polling or callback
  // failure begins stopping and returns false.
  bool PollTrading() noexcept;
  // Processes up to kMaxStrategyBatchSize ready operations fairly across the
  // three sources. A yielding market source is skipped for the remaining batch;
  // an error ends the batch. Preserves rotation across calls to avoid starving
  // a ready source.
  void ProcessBatch() noexcept;
  // Returns the earliest work or shutdown deadline in clock's absolute
  // monotonic nanoseconds. nullopt means no deadline, including outside
  // kRunning and kStopping; environment stop requests are checked separately.
  std::optional<uint64_t> next_deadline() const noexcept;
  // Enters a callback phase and enables trading requests only for business
  // callbacks while kRunning. Requires that no callback is already active;
  // each call must be paired with EndCallback.
  void BeginCallback(strategy::CallbackPhase phase) noexcept;
  // Leaves the callback phase and closes trading requests until the next
  // eligible callback. Requires a matching BeginCallback.
  void EndCallback() noexcept;
  // Checks environment stop requests and begins shutdown when requested.
  // Records environment errors and disables further trading drain work.
  void CheckStop() noexcept;
  // Begins stopping, closes new requests, disarms timers, stops market data,
  // and establishes the drain deadline. Repeated calls in kStopping or
  // kFinished leave the existing deadline and resources unchanged.
  void EnterStopping() noexcept;
  // Disables business callbacks and begins stopping after an error. If
  // can_drain is false, finalization need not wait for trading to drain.
  void StopAfterError(bool can_drain) noexcept;
  // Returns whether the trading service has no outstanding work to drain.
  bool IsDrained() const noexcept;
  // Finalizes a stopping runner when trading is drained, draining is unsafe,
  // or the deadline is due. Records a timeout error when needed. Returns true
  // if this call finalized the runner, false if stopping is not yet complete
  // or the runner is not kStopping.
  bool FinishStoppingIfReady() noexcept;
  // Clears the operation result and diagnostics without changing lifecycle
  // state, resources, or counters.
  void ResetError() noexcept;
  // Records a non-OK status only if no failure exists since ResetError.
  // Returns whether it was recorded; the caller then writes error_detail_.
  // Requires a non-null stage and borrows it until diagnostics are reset.
  bool TryRecordFailure(base::Status status, const char* stage) noexcept;
  // Ignores OK statuses and preserves any failure recorded since ResetError.
  // Requires non-null C strings. For the first failure, borrows stage until
  // diagnostics are reset and copies detail, truncating if necessary.
  void SetErrorIfNotOK(base::Status status, const char* stage,
                       const char* detail) noexcept;
  // Records the first failure with tick's source, instrument, and sequence.
  // Requires a non-OK status. Skips later failures without formatting. Borrows
  // tick and non-null C-string detail only for this call; detail must not
  // overlap error_detail_. stage follows SetErrorIfNotOK's lifetime
  // requirement.
  void SetQuoteError(base::Status status, const char* stage, const char* detail,
                     const model::QuoteTick& tick) noexcept;
  // Records the first failure with the client order ID and report sequence.
  // Requires a non-OK status. Skips later failures without formatting. Borrows
  // non-null C-string detail only for this call; it must not overlap
  // error_detail_.
  // stage follows SetErrorIfNotOK's lifetime requirement.
  void SetOrderError(base::Status status, const char* stage, const char* detail,
                     model::ClientOrderId id, uint64_t sequence) noexcept;
  // Writes outstanding order/cancel diagnostics and available account/position
  // snapshots to stderr. Requires the trading view to remain available.
  void ReportUnconfirmed() const noexcept;
  // Releases resources without a drain loop. Calls OnStop once if OnStart
  // succeeded, destroys the instance before closing its library, then resets
  // services and enters kFinished. Preserves the first error and keeps trading
  // views available through OnStop and unconfirmed-order reporting. Repeated
  // calls after kFinished do nothing.
  void Finalize() noexcept;

  // Diagnostic buffer capacity in bytes, including the terminating null.
  static constexpr size_t kErrorDetailSize = 1024;
  const base::MonotonicClock& clock_;
  base::EventLoopEnvironment& environment_;
  MarketDataSource& source_;
  trading::TradingService& trading_service_;
  // Callback-facing services and their context live through plugin destruction.
  MarketDataImpl market_data_;
  TimerImpl timer_;
  StrategyContextImpl context_;
  StrategyLibrary library_;
  strategy::StrategyId strategy_id_;  // Owned ID retained after teardown.
  // Owned through the loaded plugin's destroy function; null before creation
  // and after destruction. The library must remain open while this is non-null.
  strategy::Strategy* instance_ = nullptr;
  RunnerState state_ = RunnerState::kIdle;
  bool started_ = false;  // OnStart succeeded and OnStop is still owed.
  // Requires source_.Stop after any Prepare attempt, including failure.
  bool source_prepared_ = false;
  bool source_started_ = false;
  // Requires trading_service_.Stop after any Init attempt, including failure.
  bool trading_prepared_ = false;
  bool trading_started_ = false;
  bool callback_active_ = false;
  bool callbacks_enabled_ = true;  // Business callbacks only; excludes OnStop.
  bool drain_viable_ = true;  // False permits finalization without draining.
  // 0=market, 1=trading, 2=timer. Retained across batches so a continually
  // rearmed immediate timer cannot starve another ready source.
  uint8_t next_event_source_ = 0;
  uint64_t shutdown_timeout_ns_ = trading::kDefaultTradingTimeoutNs;
  // Absolute monotonic nanoseconds; absent until the drain deadline is set.
  std::optional<uint64_t> shutdown_deadline_;
  uint64_t ignored_quote_count_ = 0;
  base::Status result_;
  // Borrowed stage identifier, valid until reset; empty if no error is stored.
  const char* error_stage_ = "";
  char error_detail_[kErrorDetailSize] = {};
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_STRATEGY_RUNNER_H_
