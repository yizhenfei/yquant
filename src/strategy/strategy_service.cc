#include "yquant/strategy/strategy_service.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_set>
#include <utility>

#include "yquant/strategy/loaded_strategy.h"

namespace yquant::strategy {

StrategyService::StrategyService(std::uint64_t caller, market::MarketClient& market,
                                 trading::TradingClient& trading)
    : market_(market), trading_(trading), ids_(caller), subscription_client_(*this) {}

StrategyService::~StrategyService() {
  if (!Stop()) std::abort();
}

void StrategyService::AddListener(std::uint32_t instrument,
                                  market::QuoteListener& listener) {
  auto& subscribers = listeners_[instrument];
  if (std::find(subscribers.begin(), subscribers.end(), &listener) == subscribers.end()) {
    subscribers.push_back(&listener);
  }
}

void StrategyService::RemoveListener(std::uint32_t instrument,
                                     market::QuoteListener& listener) {
  const auto it = listeners_.find(instrument);
  if (it == listeners_.end()) return;
  std::erase(it->second, &listener);
  if (it->second.empty()) {
    if (active_) market_.Unsubscribe(instrument, *this);
    listeners_.erase(it);
  }
}

bool StrategyService::Load(std::span<const StrategyConfig> configs, std::string& error) {
  if (active_ || dispatching_ || configs.empty()) {
    error = "Strategy service cannot load this group";
    return false;
  }
  std::unordered_set<std::uint64_t> ids;
  for (const auto& config : configs) {
    if (!config.strategy_id || !ids.insert(config.strategy_id).second) {
      error = "Invalid or duplicate strategy ID: " + std::to_string(config.strategy_id);
      return false;
    }
  }
  listeners_.reserve(configs.size());
  strategies_.reserve(configs.size());
  for (const auto& config : configs) {
    auto strategy = LoadedStrategy::Load(config, subscription_client_, trading_, ids_, error);
    if (!strategy) {
      strategies_.clear();
      return false;
    }
    strategies_.push_back(std::move(strategy));
  }
  for (const auto& [instrument, unused] : listeners_) {
    market_.Subscribe(instrument, *this);
  }
  active_ = true;
  error.clear();
  return true;
}

bool StrategyService::Stop() {
  if (dispatching_) return false;
  // Every instance shares one trading client. On the single business thread,
  // a busy client rejects the first detach before any binding is changed.
  for (const auto& strategy : strategies_) {
    if (!strategy->Detach()) return false;
  }
  active_ = false;
  strategies_.clear();
  return true;
}

void StrategyService::OnQuote(const model::Quote& quote) {
  if (!active_ || dispatching_) return;
  const auto found = listeners_.find(quote.instrument_id);
  if (found == listeners_.end()) return;
  dispatching_ = true;
  for (auto* listener : found->second) listener->OnQuote(quote);
  dispatching_ = false;
}

}  // namespace yquant::strategy
