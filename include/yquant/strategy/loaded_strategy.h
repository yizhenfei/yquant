#ifndef YQUANT_STRATEGY_LOADED_STRATEGY_H_
#define YQUANT_STRATEGY_LOADED_STRATEGY_H_

#include <memory>
#include <vector>

#include "yquant/strategy/strategy.h"

namespace yquant::strategy {

// Loads a strategy plugin and manages its instance, subscriptions and unloading.
// Load, Detach, Stop and destruction run on the business thread, outside
// service callbacks. Keep the injected clients and ID generator available until
// the instance has stopped.
class LoadedStrategy final : public market::QuoteListener {
 public:
  // Loads the configured plugin and subscribes to the instruments it declares.
  // Returns nullptr and sets error if configuration, loading or creation fails.
  static std::unique_ptr<LoadedStrategy> Load(const std::string& config_path,
      market::MarketClient& market, trading::TradingClient& trading,
      trading::OrderIdGenerator& ids, std::string& error);
  // Loads from parsed settings with the same result and error behavior.
  static std::unique_ptr<LoadedStrategy> Load(const StrategyConfig& config,
      market::MarketClient& market, trading::TradingClient& trading,
      trading::OrderIdGenerator& ids, std::string& error);
  ~LoadedStrategy() override;
  LoadedStrategy(const LoadedStrategy&) = delete;
  LoadedStrategy& operator=(const LoadedStrategy&) = delete;
  // Detaches retained trading callbacks, unsubscribes quotes, destroys the
  // instance and unloads the library. Returns false during a callback or
  // service operation, leaving the instance running. Repeated Stop succeeds.
  bool Stop();
  // Stops quote and trading callbacks without destroying the instance.
  // Returns false if a callback is active or the trading client cannot detach.
  // Used to detach all instances before destroying any member of a group.
  bool Detach();
  const StrategyConfig& config() const { return config_; }

  // market::QuoteListener implementation.
  void OnQuote(const model::Quote& quote) override;

 private:
  LoadedStrategy(market::MarketClient& market, trading::TradingClient& trading)
      : market_(market), trading_(trading) {}
  market::MarketClient& market_;
  trading::TradingClient& trading_;
  StrategyConfig config_;
  void* library_ = nullptr;
  const StrategyPlugin* plugin_ = nullptr;
  Strategy* instance_ = nullptr;
  std::vector<std::uint32_t> subscriptions_;
  bool dispatching_ = false, detached_ = false;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_LOADED_STRATEGY_H_
