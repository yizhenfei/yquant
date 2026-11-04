#include "yquant/strategy/plugin.h"

#include <charconv>

#include "yquant/strategy/market_maker.h"

namespace yquant::strategy {
namespace {

Strategy* Create(const StrategyConfig& config, trading::TradingClient& client,
                  trading::OrderIdGenerator& ids, std::string& error) {
  const auto quantity = config.parameters.find("quantity");
  const auto instrument = config.parameters.find("instrument_id");
  if (quantity == config.parameters.end() || instrument == config.parameters.end() ||
      config.parameters.size() != 2) {
    error = "MarketMaker requires quantity and instrument_id parameters";
    return nullptr;
  }
  std::int32_t lots = 0;
  std::uint32_t instrument_id = 0;
  const auto parse = [](const std::string& text, auto& output) {
    const auto end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, output);
    return ec == std::errc{} && ptr == end && output > 0;
  };
  if (!parse(quantity->second, lots) || !parse(instrument->second, instrument_id)) {
    error = "MarketMaker quantity and instrument_id must be positive integers within range";
    return nullptr;
  }
  return new MarketMaker(client, ids, config.account_id, config.strategy_id,
                          instrument_id, lots);
}

void Destroy(Strategy* strategy) { delete strategy; }

const StrategyPlugin kPlugin{kStrategyPluginVersion, Create, Destroy};

}  // namespace
}  // namespace yquant::strategy

extern "C" const yquant::strategy::StrategyPlugin* yquant_strategy_plugin() {
  return &yquant::strategy::kPlugin;
}
