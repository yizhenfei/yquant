#ifndef YQUANT_STRATEGY_MARKET_MAKER_H_
#define YQUANT_STRATEGY_MARKET_MAKER_H_

#include <cstdint>

#include "yquant/strategy/plugin.h"
#include "yquant/trading/order_id_generator.h"
#include "yquant/trading/trading_client.h"

namespace yquant::strategy {

// Quotes one instrument on the business thread. The owner subscribes the
// strategy service to instrument_id and keeps this listener alive until trading
// stops. client and the caller service's shared ID generator are borrowed.
class MarketMaker final : public Strategy {
 public:
  MarketMaker(trading::TradingClient& client, trading::OrderIdGenerator& ids,
              std::uint64_t account_id, std::uint64_t strategy_id,
              std::uint32_t instrument_id, std::int32_t quantity);
  MarketMaker(const MarketMaker&) = delete;
  MarketMaker& operator=(const MarketMaker&) = delete;

  // market::QuoteListener implementation.
  void OnQuote(const model::Quote& quote) override;

  // trading::TradingListener implementation.
  void OnOrderReport(const trading::OrderReport& report) override;
  void OnTradeReport(const trading::TradeReport& report) override;
  void OnCancelReport(const trading::CancelReport& report) override;
  void OnRequestFailure(const trading::RequestFailure& failure) override;

 private:
  enum class Phase { kIdle, kSubmitting, kWorking, kCanceling };

  // Tracks one side's latest target and the order that may still execute.
  // A failed target is retried only after its price changes.
  struct Leg {
    Phase phase = Phase::kIdle;
    trading::OrderId id{};
    double target = 0;
    double price = 0;
    double failed_target = 0;
    double cancel_target = 0;
    std::uint64_t cancel_id = 0;
    bool terminal = false;
  };

  // Restores at most one working quote per side before placing any new quote.
  // An ambiguous set stays unbound and is reconsidered on the next quote.
  bool AdoptWorkingOrders();
  // Acts on the latest prices, using at most one live order per side.
  void Requote();
  void Advance(Leg& leg, const Leg& opposite, trading::Side side);
  // Returns the matching non-idle leg, or nullptr for a superseded order.
  Leg* Find(trading::OrderId id);
  // Retires an order without discarding the latest requested price.
  static void Retire(Leg& leg, double failed_target = 0);
  void Apply(const trading::OrderSnapshot& snapshot);

  trading::TradingClient& client_;
  trading::OrderIdGenerator& ids_;
  std::uint64_t account_;
  std::uint64_t strategy_;
  std::uint32_t instrument_;
  std::int32_t quantity_;
  bool adopted_ = false;
  Leg bid_;
  Leg ask_;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_MARKET_MAKER_H_
