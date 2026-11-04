#include "yquant/strategy/strategy_service.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "yquant/strategy/loaded_strategy.h"

namespace yquant::strategy {

// Collects private subscriptions during construction. Only Activate publishes
// the service to the upstream client, once per instrument. Instance pointers
// remain stable until the group has been detached.
struct StrategyService::Instances final : market::MarketClient {
  Instances(StrategyService& owner, std::uint64_t caller, market::MarketClient& market,
            trading::TradingClient& trading)
      : owner(owner), market(market), trading(trading), ids(caller) {}

  // market::MarketClient implementation.
  void Subscribe(std::uint32_t instrument, market::QuoteListener& listener) override {
    auto& subscribers = listeners[instrument];
    if (std::find(subscribers.begin(), subscribers.end(), &listener) == subscribers.end()) {
      subscribers.push_back(&listener);
    }
  }
  void Unsubscribe(std::uint32_t instrument, market::QuoteListener& listener) override {
    const auto it = listeners.find(instrument);
    if (it == listeners.end()) return;
    std::erase(it->second, &listener);
    if (it->second.empty()) {
      if (active) market.Unsubscribe(instrument, owner);
      listeners.erase(it);
    }
  }

  void Activate() {
    for (const auto& [instrument, unused] : listeners) market.Subscribe(instrument, owner);
    active = true;
  }

  StrategyService& owner;
  market::MarketClient& market;
  trading::TradingClient& trading;
  trading::OrderIdGenerator ids;
  std::unordered_map<std::uint32_t, std::vector<market::QuoteListener*>> listeners;
  std::vector<std::unique_ptr<LoadedStrategy>> strategies;
  bool active = false, dispatching = false;
};

StrategyService::StrategyService(market::QuoteListener& strategy) : borrowed_(&strategy) {}

StrategyService::StrategyService(std::uint64_t caller, market::MarketClient& market,
                                 trading::TradingClient& trading)
    : instances_(std::make_unique<Instances>(*this, caller, market, trading)) {}

StrategyService::~StrategyService() {
  if (!Stop()) std::abort();
}

bool StrategyService::Load(std::span<const StrategyConfig> configs, std::string& error) {
  if (!instances_ || instances_->active || instances_->dispatching || configs.empty()) {
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
  auto& state = *instances_;
  state.listeners.reserve(configs.size());
  state.strategies.reserve(configs.size());
  for (const auto& config : configs) {
    auto strategy = LoadedStrategy::Load(config, state, state.trading, state.ids, error);
    if (!strategy) {
      state.strategies.clear();
      return false;
    }
    state.strategies.push_back(std::move(strategy));
  }
  state.Activate();
  error.clear();
  return true;
}

bool StrategyService::Stop() {
  if (!instances_) return true;
  auto& state = *instances_;
  if (state.dispatching) return false;
  // Every instance shares one trading client. On the single business thread,
  // a busy client rejects the first detach before any binding is changed.
  for (const auto& strategy : state.strategies) {
    if (!strategy->Detach()) return false;
  }
  state.active = false;
  state.strategies.clear();
  return true;
}

void StrategyService::OnQuote(const model::Quote& quote) {
  if (borrowed_) { borrowed_->OnQuote(quote); return; }
  auto& state = *instances_;
  if (!state.active || state.dispatching) return;
  const auto found = state.listeners.find(quote.instrument_id);
  if (found == state.listeners.end()) return;
  state.dispatching = true;
  for (auto* listener : found->second) listener->OnQuote(quote);
  state.dispatching = false;
}

}  // namespace yquant::strategy
