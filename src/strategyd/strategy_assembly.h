#ifndef YQUANT_STRATEGYD_STRATEGY_ASSEMBLY_H_
#define YQUANT_STRATEGYD_STRATEGY_ASSEMBLY_H_

// Defines CLI startup inputs and configuration ownership for runner assembly.
// Strategy execution and shutdown remain runner duties.

#include "strategyd/replay_market_data_source.h"
#include "strategyd/shm_market_data_source.h"
#include "strategyd/strategy_runner.h"
#include "strategyd/trading_service_impl.h"

namespace yquant::strategyd {

// Configuration paths for one assembly load. All paths are required, non-null,
// null-terminated strings. Load borrows configuration paths only for the call;
// library_path must remain valid through the runner's LoadAndStart call.
struct StrategyOptions {
  const char* library_path;
  const char* config_path;
  const char* market_data_config_path = nullptr;
  const char* trading_config_path = nullptr;
};

// Loads startup configuration and owns the selected market source and tradingd
// client used by a runner. Load once, check both its status and cancelled(),
// then borrow start_options and components to construct and start the runner.
//
// All assembly and component access is serialized on the owning strategy
// thread. Destroy the runner before this owner so its borrowed services remain
// alive during shutdown.
class StrategyAssembly {
 public:
  // Borrows clock until destruction through the owned trading client.
  explicit StrategyAssembly(const base::MonotonicClock& clock) noexcept
      : trading_(clock, instruments_) {}

  // Loads and validates all configuration, checking environment between stages.
  // Retains the plugin path; other option strings and environment are borrowed
  // only during this call. Does not activate a source or trading connection.
  //
  // Cancellation returns OK unless checking the environment fails; cancelled()
  // is true in either case. Failure may retain partial configuration for
  // diagnostics. Call start_options, source, and trading only after OK and
  // !cancelled(). A second Load returns kInvalidState without changing state.
  [[nodiscard]] base::Status Load(
      StrategyOptions options,
      base::EventLoopEnvironment& environment) noexcept;
  bool cancelled() const noexcept { return cancelled_; }
  // Returns inputs borrowing this owner's configuration and the caller's
  // plugin path. LoadAndStart may reset the borrowed strategy configuration.
  StrategyStartOptions start_options() noexcept {
    return {library_path_, strategy_config_, market_config_.config(),
            trading_config_.config()};
  }
  // Returns the configured source, borrowed until this owner is destroyed.
  MarketDataSource& source() noexcept {
    return market_config_.config().adapter == md::MarketDataAdapter::kReplay
               ? static_cast<MarketDataSource&>(replay_source_)
               : static_cast<MarketDataSource&>(source_);
  }
  // Returns the owned trading client; the caller must not outlive this owner.
  trading::TradingService& trading() noexcept { return trading_; }
  // Returns the copied ID, retained even after the runner consumes its config.
  std::string_view strategy_id() const noexcept { return strategy_id_.view(); }
  // Returns non-null diagnostic strings, valid until destruction. A successful
  // load or clean cancellation leaves them empty; failed loads retain details.
  const char* error_stage() const noexcept { return error_stage_; }
  const char* error_detail() const noexcept { return error_detail_; }

 private:
  // Stores a loader diagnostic and returns status unchanged. Requires non-null
  // stage valid through destruction; copies error's text into bounded storage.
  base::Status ConfigFailure(base::Status status, const char* stage,
                             const base::ConfigError& error) noexcept;

  // Diagnostic capacity in bytes, including the terminating null.
  static constexpr std::size_t kErrorDetailSize = 1024;
  StrategyConfigLoader strategy_config_;
  md::MarketDataConfigLoader market_config_;
  trading::TradingClientConfigLoader trading_config_;
  md::InstrumentScope instruments_;
  ShmMarketDataSource source_;
  ReplayMarketDataSource replay_source_;
  TradingServiceImpl trading_;
  strategy::StrategyId strategy_id_;
  // Borrowed from StrategyOptions; null until Load reaches path assignment.
  const char* library_path_ = nullptr;
  bool loaded_ = false;  // Load was attempted, including failure/cancellation.
  bool cancelled_ = false;
  const char* error_stage_ = "";  // Borrowed static stage name, never null.
  char error_detail_[kErrorDetailSize] = {};
};

}  // namespace yquant::strategyd
#endif  // YQUANT_STRATEGYD_STRATEGY_ASSEMBLY_H_
