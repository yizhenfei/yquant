#ifndef YQUANT_STRATEGY_LOADED_STRATEGY_H_
#define YQUANT_STRATEGY_LOADED_STRATEGY_H_

#include <memory>

#include "yquant/strategy/plugin.h"

namespace yquant::strategy {

// Owns an instance and its library on the business thread. The injected clients,
// their services and the shared ID generator must outlive this object. Destroy
// it between service calls, after stopping the container's scheduling loop.
class LoadedStrategy final : public market::QuoteListener {
 public:
  static std::unique_ptr<LoadedStrategy> Load(const std::string& config_path,
      market::MarketClient& market, trading::TradingClient& trading,
      trading::OrderIdGenerator& ids, std::string& error);
  // Uses an already parsed immutable configuration for process assembly.
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
  bool subscribed_ = false, dispatching_ = false;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_LOADED_STRATEGY_H_
