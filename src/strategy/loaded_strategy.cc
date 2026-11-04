#include "yquant/strategy/loaded_strategy.h"

#include <dlfcn.h>

#include <cstdlib>

namespace yquant::strategy {

std::unique_ptr<LoadedStrategy> LoadedStrategy::Load(const std::string& config_path,
    const market::MarketClientFactory& markets, std::uint64_t market_id,
    const trading::TradingClientFactory& traders, std::uint64_t trading_id,
    trading::OrderIdGenerator& ids, std::string& error) {
  StrategyConfig config;
  if (!ReadStrategyConfig(config_path, config, error)) return nullptr;
  return Load(config, markets, market_id, traders, trading_id, ids, error);
}

std::unique_ptr<LoadedStrategy> LoadedStrategy::Load(const StrategyConfig& config,
    const market::MarketClientFactory& markets, std::uint64_t market_id,
    const trading::TradingClientFactory& traders, std::uint64_t trading_id,
    trading::OrderIdGenerator& ids, std::string& error) {
  if (config.library.empty() || !config.strategy_id || !config.account_id) {
    error = "Incomplete strategy configuration";
    return nullptr;
  }
  auto loaded = std::unique_ptr<LoadedStrategy>(new LoadedStrategy());
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
  loaded->market_ = markets.Create(market_id);
  if (!loaded->market_) { error = "Market service is unavailable for client creation"; return nullptr; }
  loaded->trading_ = traders.Create(trading_id);
  if (!loaded->trading_) { error = "Trading service is unavailable for client creation"; return nullptr; }
  loaded->context_.emplace(loaded->config_, *loaded->market_, *loaded->trading_, ids);
  loaded->instance_ = api->create(*loaded->context_, error);
  if (!loaded->instance_) {
    if (error.empty()) error = "Strategy creation failed: " + loaded->config_.library;
    return nullptr;
  }
  if (!loaded->market_->BindListener(*loaded->instance_) ||
      !loaded->trading_->BindListener(*loaded->instance_)) {
    error = "Strategy must not bind its own client listeners";
    return nullptr;
  }
  error.clear();
  return loaded;
}

LoadedStrategy::~LoadedStrategy() {
  if (!Stop()) std::abort();  // Destruction during callbacks violates the lifetime contract.
}

bool LoadedStrategy::Initialize(std::string& error) {
  if (!instance_ || detached_ || initializing_) {
    error = "Strategy cannot initialize in its current state";
    return false;
  }
  if (initialized_) { error.clear(); return true; }
  initializing_ = true;
  const bool ok = instance_->OnInitialize(error);
  initializing_ = false;
  if (!ok) {
    if (error.empty()) error = "Strategy initialization failed: " + config_.library;
    Stop();
    return false;
  }
  initialized_ = true;
  error.clear();
  return true;
}

bool LoadedStrategy::CanStop() const {
  return !initializing_ && (!market_ || market_->CanClose()) && (!trading_ || trading_->CanClose());
}

bool LoadedStrategy::Detach() {
  if (detached_) return true;
  if (!CanStop()) return false;
  if (market_ && !market_->Close()) return false;
  if (trading_ && !trading_->Close()) return false;
  detached_ = true;
  return true;
}

bool LoadedStrategy::Stop() {
  if (!Detach()) return false;
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

}  // namespace yquant::strategy
