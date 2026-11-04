#include "yquant/strategy/loaded_strategy.h"

#include <dlfcn.h>

#include <cstdlib>

namespace yquant::strategy {

std::unique_ptr<LoadedStrategy> LoadedStrategy::Load(const std::string& config_path,
    market::MarketClient& market, trading::TradingClient& trading,
    trading::OrderIdGenerator& ids, std::string& error) {
  StrategyConfig config;
  if (!ReadStrategyConfig(config_path, config, error)) return nullptr;
  return Load(config, market, trading, ids, error);
}

std::unique_ptr<LoadedStrategy> LoadedStrategy::Load(const StrategyConfig& config,
    market::MarketClient& market, trading::TradingClient& trading,
    trading::OrderIdGenerator& ids, std::string& error) {
  if (config.library.empty() || !config.strategy_id || !config.account_id || !config.instrument_id) {
    error = "Incomplete strategy configuration";
    return nullptr;
  }
  auto loaded = std::unique_ptr<LoadedStrategy>(new LoadedStrategy(market, trading));
  loaded->config_ = config;
  loaded->library_ = dlopen(loaded->config_.library.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!loaded->library_) { error = dlerror(); return nullptr; }
  dlerror();
  auto entry = reinterpret_cast<StrategyPluginEntry>(
      dlsym(loaded->library_, "yquant_strategy_plugin"));
  if (const auto* message = dlerror()) { error = message; return nullptr; }
  loaded->plugin_ = entry ? entry() : nullptr;
  const auto* api = loaded->plugin_;
  if (!api || api->version != kStrategyPluginVersion || !api->create || !api->destroy) {
    error = "Incompatible strategy plugin interface: " + loaded->config_.library;
    return nullptr;
  }
  loaded->instance_ = api->create(loaded->config_, trading, ids, error);
  if (!loaded->instance_) {
    if (error.empty()) error = "Strategy creation failed: " + loaded->config_.library;
    return nullptr;
  }
  market.Subscribe(loaded->config_.instrument_id, *loaded);
  loaded->subscribed_ = true;
  error.clear();
  return loaded;
}

LoadedStrategy::~LoadedStrategy() {
  if (!Stop()) std::abort();  // Destruction during callbacks violates the lifetime contract.
}

bool LoadedStrategy::Stop() {
  if (dispatching_) return false;
  if (instance_ && !trading_.DetachListener(*instance_)) return false;
  if (subscribed_) {
    market_.Unsubscribe(config_.instrument_id, *this);
    subscribed_ = false;
  }
  if (instance_) {
    plugin_->destroy(instance_);
    instance_ = nullptr;
  }
  if (library_) {
    dlclose(library_);
    library_ = nullptr;
    plugin_ = nullptr;
  }
  return true;
}

void LoadedStrategy::OnQuote(const model::Quote& quote) {
  if (!instance_ || dispatching_) return;
  dispatching_ = true;
  instance_->OnQuote(quote);
  dispatching_ = false;
}

}  // namespace yquant::strategy
