#include "yquant/strategy/strategy_service.h"

#include <cstdlib>
#include <unordered_set>
#include <utility>

namespace yquant::strategy {

StrategyService::StrategyService(std::uint64_t caller,
    const market::MarketClientFactory& markets, std::uint64_t market_id,
    const trading::TradingClientFactory& traders, std::uint64_t trading_id)
    : markets_(markets), traders_(traders), market_id_(market_id),
      trading_id_(trading_id), ids_(caller) {}

StrategyService::~StrategyService() {
  if (!Stop()) std::abort();
}

bool StrategyService::Load(std::span<const StrategyConfig> configs, std::string& error) {
  if (!strategies_.empty() || configs.empty()) {
    error = "Strategy service cannot load this group";
    return false;
  }
  std::unordered_set<std::uint64_t> ids;
  for (const auto& config : configs) {
    if (!config.strategy_id || !ids.insert(config.strategy_id).second) {
      error = "Invalid or duplicate strategy ID";
      return false;
    }
  }
  std::unordered_set<std::uint32_t> instruments;
  strategies_.reserve(configs.size());
  for (const auto& config : configs) {
    auto strategy = LoadedStrategy::Load(config, markets_, market_id_, traders_, trading_id_, ids_, error);
    if (!strategy) { Stop(); return false; }
    for (auto instrument : strategy->subscriptions()) {
      if (instruments.insert(instrument).second) subscriptions_.push_back(instrument);
    }
    strategies_.push_back(std::move(strategy));
  }
  for (const auto& strategy : strategies_) {
    if (!strategy->Start()) {
      error = "Could not start strategy subscriptions";
      Stop();
      return false;
    }
  }
  error.clear();
  return true;
}

bool StrategyService::Stop() {
  for (const auto& strategy : strategies_) if (!strategy->CanStop()) return false;
  for (const auto& strategy : strategies_) if (!strategy->Detach()) return false;
  strategies_.clear();
  subscriptions_.clear();
  return true;
}

}  // namespace yquant::strategy
