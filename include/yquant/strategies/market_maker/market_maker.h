#ifndef YQUANT_STRATEGIES_MARKET_MAKER_MARKET_MAKER_H_
#define YQUANT_STRATEGIES_MARKET_MAKER_MARKET_MAKER_H_

#include <cstdint>

#include "yquant/strategy/strategy.h"
#include "yquant/trading/order_id_generator.h"
#include "yquant/trading/trading_client.h"

namespace yquant::strategies::market_maker {

// Maintains buy and sell quotes for one instrument at the best bid and ask.
// Cancels an old quote before replacing it. Run callbacks on the business thread
// and share the caller service's ID generator with its other strategy instances.
class MarketMaker final : public strategy::Strategy {
 public:
  MarketMaker(strategy::StrategyContext& context,
              std::uint32_t instrument_id, std::int32_t quantity);
  MarketMaker(const MarketMaker&) = delete;
  MarketMaker& operator=(const MarketMaker&) = delete;

  // strategy::Strategy implementation.
  bool OnInitialize(std::string& error) override;

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
  // Returns false if the working orders cannot be adopted; retries on a later quote.
  bool AdoptWorkingOrders();
  // Acts on the latest prices, using at most one live order per side.
  void Requote();
  void Advance(Leg& leg, const Leg& opposite, trading::Side side);
  // Returns the matching non-idle leg, or nullptr for a superseded order.
  Leg* Find(trading::OrderId id);
  // Retires an order without discarding the latest requested price.
  static void Retire(Leg& leg, double failed_target = 0);
  void Apply(const trading::OrderSnapshot& snapshot);

  strategy::StrategyContext& context_;
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

}  // namespace yquant::strategies::market_maker

#endif  // YQUANT_STRATEGIES_MARKET_MAKER_MARKET_MAKER_H_
