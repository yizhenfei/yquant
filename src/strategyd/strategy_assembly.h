#ifndef YQUANT_STRATEGYD_STRATEGY_ASSEMBLY_H_
#define YQUANT_STRATEGYD_STRATEGY_ASSEMBLY_H_

#include "strategyd/shm_market_data_source.h"
#include "strategyd/strategy_runner.h"
#include "strategyd/trading_service_impl.h"

namespace yquant::strategyd {

// CLI inputs. library_path remains valid through Handle(start_command());
// all other paths are borrowed only during Load.
struct StrategyOptions {
  const char* library_path;
  const char* config_path;
  const char* market_data_config_path = nullptr;
  const char* trading_config_path = nullptr;
};

// Owns validated startup configuration, SHM receiver and tradingd client.
// Load and all component operations are serialized on the main thread. Clock
// outlives this owner; the runner must be destroyed first.
class StrategyAssembly {
 public:
  explicit StrategyAssembly(const base::MonotonicClock& clock) noexcept
      : trading_(clock, instruments_) {}

  // Single use. Checks cancellation between file reads. Failure/cancellation
  // acquires no active source/transport. Only after success and !cancelled()
  // may callers borrow components and start_command(). Diagnostics are owned.
  [[nodiscard]] base::Status Load(
      StrategyOptions options,
      base::EventLoopEnvironment& environment) noexcept;
  bool cancelled() const noexcept { return cancelled_; }
  LoadAndStart start_command() noexcept {
    return {library_path_, strategy_config_, market_config_.config(),
            trading_config_.config()};
  }
  MarketDataSource& source() noexcept { return source_; }
  trading::TradingService& trading() noexcept { return trading_; }
  std::string_view strategy_id() const noexcept { return strategy_id_.view(); }
  const char* error_stage() const noexcept { return error_stage_; }
  const char* error_detail() const noexcept { return error_detail_; }

 private:
  // Copies a loader's field diagnostic before it goes out of scope.
  base::Status ConfigFailure(base::Status status, const char* stage,
                             const base::ConfigError& error) noexcept;

  static constexpr std::size_t kErrorDetailSize = 1024;
  StrategyConfigLoader strategy_config_;
  md::MarketDataConfigLoader market_config_;
  trading::TradingClientConfigLoader trading_config_;
  md::InstrumentScope instruments_;
  ShmMarketDataSource source_;
  TradingServiceImpl trading_;
  strategy::StrategyId strategy_id_;
  const char* library_path_ = nullptr;
  bool loaded_ = false;
  bool cancelled_ = false;
  const char* error_stage_ = "";
  char error_detail_[kErrorDetailSize] = {};
};

}  // namespace yquant::strategyd
#endif  // YQUANT_STRATEGYD_STRATEGY_ASSEMBLY_H_
