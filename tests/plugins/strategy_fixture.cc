#include "yquant/strategy/strategy.h"

#include <charconv>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace yquant::strategy {
namespace {

// Sends one request at a configured price and records destruction outside the
// module so tests can check cleanup after the last dlclose.
class FixedPrice final : public Strategy {
 public:
  FixedPrice(StrategyContext& context, double price)
      : context_(context), config_(context.config()), client_(context.trading_client()),
        ids_(context.order_ids()), price_(price) {
    const auto& config = context.config();
    const auto setting = config.parameters.find("instrument_id");
    std::istringstream input(setting == config.parameters.end() ? "" : setting->second);
    std::uint32_t instrument;
    while (input >> instrument) instruments_.push_back(instrument);
  }
  ~FixedPrice() override {
    auto marker = config_.parameters.find("marker");
    if (marker != config_.parameters.end()) std::ofstream(marker->second) << "destroyed";
  }

  // Strategy implementation.
  bool OnInitialize(std::string& error) override {
    if (client_.BindListener(*this)) {
      error = "Trading listener was not bound before initialization";
      return false;
    }
    const auto marker = config_.parameters.find("marker");
    if (marker != config_.parameters.end()) {
      std::ofstream(marker->second + ".initialized", std::ios::app) << "initialized\n";
    }
    for (auto instrument : instruments_) {
      if (!context_.market_client().Subscribe(instrument)) {
        error = "fixture subscription failed";
        return false;
      }
    }
    if (config_.parameters.contains("fail_initialize")) {
      error = "fixture initialization failed after subscribing";
      return false;
    }
    return true;
  }

  // market::QuoteListener implementation.
  void OnQuote(const model::Quote& quote) override {
    if (!sent_.insert(quote.instrument_id).second) return;
    auto id = ids_.Next();
    if (id) client_.SubmitOrder({{*id, config_.account_id, config_.strategy_id,
        quote.instrument_id, trading::Side::kBuy, trading::Offset::kOpen, price_, 1}});
  }

  // trading::TradingListener implementation.
  void OnRequestFailure(const trading::RequestFailure&) override {}
  void OnOrderReport(const trading::OrderReport& report) override {
    auto marker = config_.parameters.find("marker");
    if (marker != config_.parameters.end()) {
      std::ofstream(marker->second + ".orders", std::ios::app)
          << report.snapshot.order.strategy_id << " "
          << report.snapshot.order.order_id.caller_service_id << " "
          << report.snapshot.order.order_id.sequence << "\n";
    }
  }
  void OnCancelReport(const trading::CancelReport&) override {}
  void OnTradeReport(const trading::TradeReport&) override {}

 private:
  StrategyContext& context_;
  const StrategyConfig& config_;
  trading::TradingClient& client_;
  trading::OrderIdGenerator& ids_;
  double price_;
  std::vector<std::uint32_t> instruments_;
  std::unordered_set<std::uint32_t> sent_;
};

Strategy* Create(StrategyContext& context, std::string& error) {
  const auto& config = context.config();
  const auto it = config.parameters.find("price");
  if (it == config.parameters.end()) { error = "missing price"; return nullptr; }
  double price = 0;
  const auto end = it->second.data() + it->second.size();
  auto [ptr, ec] = std::from_chars(it->second.data(), end, price);
  if (ec != std::errc{} || ptr != end || price <= 0) { error = "bad price"; return nullptr; }
  auto* instance = new FixedPrice(context, price);
#ifdef FAIL_CREATE
  delete instance;
  error = "fixture creation failed";
  return nullptr;
#else
  return instance;
#endif
}

void Destroy(Strategy* strategy) { delete strategy; }
#ifndef TEST_VERSION
#define TEST_VERSION kStrategyPluginVersion
#endif
const StrategyPlugin kPlugin{TEST_VERSION, Create, Destroy};

}  // namespace
}  // namespace yquant::strategy

extern "C" const yquant::strategy::StrategyPlugin* yquant_strategy_plugin() {
  return &yquant::strategy::kPlugin;
}
