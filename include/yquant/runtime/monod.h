#ifndef YQUANT_RUNTIME_MONOD_H_
#define YQUANT_RUNTIME_MONOD_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "yquant/market/simnow_quote_source.h"
#include "yquant/strategy/strategy.h"
#include "yquant/trading/simnow_trading_adapter.h"

namespace yquant::runtime {

// Owns the instances sharing one strategy-service caller identity.
struct StrategyGroupConfig {
  std::uint64_t caller_service_id = 0;
  std::vector<strategy::StrategyConfig> instances;
};

// Owns one process's static deployment settings. Credentials come from the
// environment in main. run_seconds=0 runs until the external stop condition.
struct MonodConfig {
  std::vector<StrategyGroupConfig> services;
  trading::SimNowInstruments instruments;
  std::uint64_t account_id = 0;
  std::string market_front, trading_front;
  int startup_seconds = 30, shutdown_seconds = 10, run_seconds = 0;
};

// Reads a key=value deployment file and every referenced strategy configuration.
// Requires unique caller/instance pairs and a shared account; output changes only
// on success. Single-instance settings become a group with one instance.
// Both library and strategy file paths resolve relative to their own file.
bool ReadMonodConfig(const std::string& path, MonodConfig& output, std::string& error);

// Assembles services and clients on one business thread. stop must not block.
// config must be a successful ReadMonodConfig result. Quotes reach strategies
// only after all plugins load and the account and market subscriptions are ready.
// Stops every instance before canceling its working orders. Returns zero only
// when all these orders have ended and their fills have been priced.
// The source and adapter stay alive until all services and listeners are gone.
int RunMonod(const MonodConfig& config, market::SimNowQuoteSource& source,
             trading::SimNowTradingAdapter& adapter, const std::function<bool()>& stop,
             std::string& error);

}  // namespace yquant::runtime

#endif  // YQUANT_RUNTIME_MONOD_H_
