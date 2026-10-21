#include "strategyd/strategy_assembly.h"

#include <cstdio>

namespace yquant::strategyd {

base::Status StrategyAssembly::ConfigFailure(
    base::Status status, const char* stage,
    const base::ConfigError& error) noexcept {
  error_stage_ = stage;
  std::snprintf(error_detail_, sizeof(error_detail_), "%s: %s%s",
                error.field_path, error.message,
                error.truncated ? " [truncated]" : "");
  return status;
}

base::Status StrategyAssembly::Load(
    StrategyOptions options, base::EventLoopEnvironment& environment) noexcept {
  if (loaded_) return {base::ErrorCode::kInvalidState, {}};
  loaded_ = true;
  // Cancellation is distinct from successful loading: no runner is created.
  const auto check_stop = [&]() noexcept {
    bool stop = false;
    const auto status = environment.CheckStop(stop);
    cancelled_ = stop || !status.ok();
    if (!status.ok()) {
      error_stage_ = "signal";
      std::snprintf(error_detail_, sizeof(error_detail_),
                    "could not inspect termination signals");
    }
    return status;
  };
  auto status = check_stop();
  if (cancelled_) return status;
  library_path_ = options.library_path;
  status = strategy_config_.Load(options.config_path);
  if (!status.ok())
    return ConfigFailure(status, "config", strategy_config_.error());
  strategy_id_ = strategy_config_.strategy_id();
  status = check_stop();
  if (cancelled_) return status;
  status = market_config_.Load(options.market_data_config_path);
  if (!status.ok())
    return ConfigFailure(status, "market_data_config", market_config_.error());
  status = instruments_.Configure(market_config_.config());
  if (!status.ok()) {
    error_stage_ = "market_data_config";
    std::snprintf(error_detail_, sizeof(error_detail_),
                  "could not prepare the instrument dictionary");
    return status;
  }
  status = check_stop();
  if (cancelled_) return status;
  if (options.trading_config_path != nullptr) {
    status = trading_config_.Load(options.trading_config_path);
    if (!status.ok())
      return ConfigFailure(status, "trading_config", trading_config_.error());
    trading_enabled_ = true;
    switch (trading_config_.config().adapter) {
      case trading::TradingAdapterKind::kMock:
        local_trading_.emplace(clock_, instruments_, local_adapter_);
        trading_ = &*local_trading_;
        break;
      case trading::TradingAdapterKind::kTcp:
        remote_trading_.emplace(clock_, instruments_);
        trading_ = &*remote_trading_;
        break;
    }
  }
  return check_stop();
}

}  // namespace yquant::strategyd
