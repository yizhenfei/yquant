#ifndef YQUANT_RUNTIME_MONOD_H_
#define YQUANT_RUNTIME_MONOD_H_

#include <cstdint>
#include <functional>
#include <string>

#include "yquant/market/simnow_quote_source.h"
#include "yquant/strategy/plugin.h"
#include "yquant/trading/simnow_trading_adapter.h"

namespace yquant::runtime {

// Owns one process's static deployment settings. Credentials come from the
// environment in main. run_seconds=0 runs until the external stop condition.
struct MonodConfig {
  strategy::StrategyConfig strategy;
  std::string market_front, trading_front, exchange, symbol;
  std::uint64_t caller_service_id = 0;
  int startup_seconds = 30, shutdown_seconds = 10, run_seconds = 0;
  trading::SimNowClosePriority close_priority = trading::SimNowClosePriority::kUnspecified;
};

// Reads a key=value deployment file and its referenced strategy configuration.
// Both library and strategy file paths resolve relative to their own file.
bool ReadMonodConfig(const std::string& path, MonodConfig& output, std::string& error);

// Assembles services and clients on one business thread. stop must not block.
// Success requires confirmed shutdown of this instance's remaining orders.
// Gate quotes until startup completes; stop the strategy before draining cancels.
// The source and adapter stay alive until all services and listeners are gone.
int RunMonod(const MonodConfig& config, market::SimNowQuoteSource& source,
             trading::SimNowTradingAdapter& adapter, const std::function<bool()>& stop,
             std::string& error);

}  // namespace yquant::runtime

#endif  // YQUANT_RUNTIME_MONOD_H_
