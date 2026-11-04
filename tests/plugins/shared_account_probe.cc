#include "yquant/strategy/plugin.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace yquant::strategy {
namespace {

// Executes configured actions on numbered quotes and records the public account
// view. Each module instance has its own output file and trading listener.
class AccountProbe final : public Strategy {
 public:
  AccountProbe(const StrategyConfig& config, trading::TradingClient& client,
               trading::OrderIdGenerator& ids)
      : config_(config), client_(client), ids_(ids),
        output_(config.parameters.at("output")) {
    std::istringstream submits(config.parameters.at("submit"));
    std::istringstream cancels(config.parameters.at("cancel"));
    std::uint64_t sequence;
    while (submits >> sequence) submits_.push_back(sequence);
    while (cancels >> sequence) cancels_.push_back(sequence);
  }

  // market::QuoteListener implementation.
  void OnQuote(const model::Quote& quote) override {
    for (auto sequence : submits_) {
      if (quote.sequence != sequence) continue;
      const auto id = ids_.Next();
      if (!id) continue;
      last_ = *id;
      const bool close = config_.parameters.at("offset") == "close";
      client_.SubmitOrder({{last_, config_.account_id, config_.strategy_id,
          config_.instrument_id, close ? trading::Side::kSell : trading::Side::kBuy,
          close ? trading::Offset::kCloseToday : trading::Offset::kOpen,
          quote.bid_price, std::stoi(config_.parameters.at("quantity"))}}, *this);
    }
    for (auto sequence : cancels_) {
      if (quote.sequence == sequence) {
        client_.CancelOrder({last_, config_.account_id, config_.strategy_id, ++cancel_id_}, *this);
      }
    }
    trading::FundsSnapshot funds;
    trading::PositionsSnapshot positions;
    const bool cash_ok = client_.QueryFunds({config_.account_id}, funds);
    const bool positions_ok = client_.QueryPositions({config_.account_id}, positions);
    int quantity = 0, frozen = 0;
    if (positions_ok) {
      for (const auto& p : positions.positions) {
        quantity += p.today_quantity + p.yesterday_quantity;
        frozen += p.today_frozen + p.yesterday_frozen;
      }
    }
    output_ << std::setprecision(17) << "view " << cash_ok << ' ' << positions_ok
            << ' ' << funds.available_for_orders() << ' ' << funds.reserved
            << ' ' << funds.commission << ' ' << funds.realized_pnl
            << ' ' << quantity << ' ' << frozen << '\n';
    output_.flush();
  }

  // trading::TradingListener implementation.
  void OnRequestFailure(const trading::RequestFailure& r) override {
    Event("failure", r.order_id, static_cast<int>(r.reason));
  }
  void OnOrderReport(const trading::OrderReport& r) override {
    Event("order", r.snapshot.order.order_id, static_cast<int>(r.snapshot.state));
  }
  void OnCancelReport(const trading::CancelReport& r) override {
    Event("cancel", r.order_id, static_cast<int>(r.result));
  }
  void OnTradeReport(const trading::TradeReport& r) override {
    Event("trade", r.trade.order_id, r.trade.quantity);
  }

 private:
  void Event(const char* kind, trading::OrderId id, int value) {
    output_ << kind << ' ' << id.caller_service_id << ' ' << id.sequence << ' '
            << config_.strategy_id << ' ' << value << '\n';
    output_.flush();
  }
  StrategyConfig config_;
  trading::TradingClient& client_;
  trading::OrderIdGenerator& ids_;
  std::ofstream output_;
  std::vector<std::uint64_t> submits_, cancels_;
  trading::OrderId last_;
  std::uint64_t cancel_id_ = 0;
};

Strategy* Create(const StrategyConfig& c, trading::TradingClient& client,
                 trading::OrderIdGenerator& ids, std::string&) {
  return new AccountProbe(c, client, ids);
}
void Destroy(Strategy* s) { delete s; }
const StrategyPlugin kPlugin{kStrategyPluginVersion, Create, Destroy};

}  // namespace
}  // namespace yquant::strategy

extern "C" const yquant::strategy::StrategyPlugin* yquant_strategy_plugin() {
  return &yquant::strategy::kPlugin;
}
