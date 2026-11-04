#include "position_holds_demo.h"

#include <array>
#include <cinttypes>
#include <cstdio>

#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::verification {
namespace {

// Only the second strategy's first attempt should be refused.
class CloseResults final : public trading::TradingListener {
 public:
  // trading::TradingListener implementation.
  void OnRequestFailure(const trading::RequestFailure& failure) override {
    if (failure.order_id == trading::OrderId{100, 2} &&
        failure.reason == trading::FailureReason::kInsufficientPosition) {
      ++refused;
    } else {
      unexpected = true;
    }
  }
  void OnOrderReport(const trading::OrderReport&) override {}
  void OnCancelReport(const trading::CancelReport&) override {}
  void OnTradeReport(const trading::TradeReport&) override {}
  int refused = 0;
  bool unexpected = false;
};

// Checks and prints the totals returned by the strategy-facing client.
bool CheckPosition(trading::TradingClient& client, const char* stage,
                   int quantity, int frozen) {
  trading::PositionInfo row;
  if (!client.QueryPosition({7, {42}}, row) ||
      row.today_quantity + row.yesterday_quantity != quantity ||
      row.today_frozen + row.yesterday_frozen != frozen ||
      row.available_quantity() != quantity - frozen) return false;
  std::printf("position_close stage=%s quantity=%d frozen=%d available=%" PRId64
              "\n", stage, quantity, frozen, row.available_quantity());
  return true;
}

// Exercises two independent clients against the same account and instrument.
bool VerifyClose() {
  trading::SimulatedTradingAdapter adapter;
  CloseResults results;
  trading::TradingService service;
  trading::TradingServiceRegistry registry;
  if (!service.BindAccount(7, adapter) || !registry.Register(3, service)) {
    return false;
  }
  trading::FundsTerms terms;
  terms.currency = model::Currency("CNY");
  terms.contract_multiplier = 1;
  terms.fees[1] = trading::FeeRate{1, 0};
  if (!service.ConfigureFundsTerms(7, 42, trading::Side::kSell, terms)) {
    return false;
  }
  const std::array<trading::PositionInfo, 1> rows{{
      {{42}, 4, 6, 0, 0, trading::PositionCost{400, 40},
       trading::PositionCost{600, 60}}}};
  adapter.SetInitialState({terms.currency, 1000, 900, 100, 0}, rows);
  service.Run();
  trading::TradingClientFactory factory(registry);
  auto first = factory.Create(3);
  auto second = factory.Create(3);
  if (!first || !second) return false;
  trading::SubmitOrderRequest request{{{100, 1}, 7, 2, 42, trading::Side::kSell,
                                      trading::Offset::kClose, 100, 7}};
  first->SubmitOrder(request, results);
  request.order.order_id.sequence = 2;
  request.order.strategy_id = 3;
  second->SubmitOrder(request, results);
  service.Run();
  if (results.refused != 1 || adapter.FindOrder({100, 2}) ||
      !CheckPosition(*second, "submitted", 10, 7)) return false;
  if (!adapter.Fill({{100, 1}, {"sim", "day", "partial"}, 100, 3})) return false;
  service.Run();
  if (!CheckPosition(*second, "partial", 7, 4)) return false;
  first->CancelOrder({{100, 1}, 7, 2, 1}, results);
  service.Run();
  if (!CheckPosition(*second, "canceled", 7, 0)) return false;
  request.order.order_id.sequence = 3;
  second->SubmitOrder(request, results);
  service.Run();
  return !results.unexpected && CheckPosition(*first, "reused", 7, 7);
}

}  // namespace

int RunPositionHoldsDemo() {
  if (VerifyClose()) return 0;
  std::fprintf(stderr, "Failed to verify shared account position holds\n");
  return 1;
}

}  // namespace yquant::verification
