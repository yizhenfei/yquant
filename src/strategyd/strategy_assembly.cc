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
  // A clean stop can return OK before configuration is complete. Preserve a
  // separate cancellation flag so the caller does not construct a runner.
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
  status = trading_config_.Load(options.trading_config_path);
  if (!status.ok())
    return ConfigFailure(status, "trading_config", trading_config_.error());
  return check_stop();
}

}  // namespace yquant::strategyd
