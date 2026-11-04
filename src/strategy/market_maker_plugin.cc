#include "yquant/strategy/plugin.h"

#include <charconv>

#include "yquant/strategy/market_maker.h"

namespace yquant::strategy {
namespace {

Strategy* Create(const StrategyConfig& config, trading::TradingClient& client,
                  trading::OrderIdGenerator& ids, std::string& error) {
  const auto quantity = config.parameters.find("quantity");
  if (quantity == config.parameters.end() || config.parameters.size() != 1) {
    error = "MarketMaker requires exactly one parameter: quantity";
    return nullptr;
  }
  std::int32_t lots = 0;
  const auto& value = quantity->second;
  const auto end = value.data() + value.size();
  const auto [ptr, ec] = std::from_chars(value.data(), end, lots);
  if (ec != std::errc{} || ptr != end || lots <= 0) {
    error = "MarketMaker quantity must be a positive integer";
    return nullptr;
  }
  return new MarketMaker(client, ids, config.account_id, config.strategy_id,
                          config.instrument_id, lots);
}

void Destroy(Strategy* strategy) { delete strategy; }

const StrategyPlugin kPlugin{kStrategyPluginVersion, Create, Destroy};

}  // namespace
}  // namespace yquant::strategy

extern "C" const yquant::strategy::StrategyPlugin* yquant_strategy_plugin() {
  return &yquant::strategy::kPlugin;
}
