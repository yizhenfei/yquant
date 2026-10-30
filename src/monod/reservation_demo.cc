#include "reservation_demo.h"

#include <cinttypes>
#include <cstdio>
#include <optional>

#include "yquant/model/currency.h"
#include "yquant/trading/funds.h"
#include "yquant/trading/funds_terms.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_listener.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::monod {
namespace {

// Distinguishes expected business refusals from transmission diagnostics.
class ReservationOutput final : public trading::TradingListener {
 public:
  // trading::TradingListener implementation.
  void OnOrderReport(const trading::OrderReport& report) override {
    if (report.snapshot.state == trading::OrderState::kAccepted) {
      ++sent;
    } else {
      ++unexpected;
    }
  }
  void OnRequestFailure(const trading::RequestFailure& failure) override {
    if (failure.reason == trading::FailureReason::kInsufficientFunds) {
      std::printf("reservation_failure sequence=%" PRIu64
                  " reason=insufficient_funds\n", failure.order_id.sequence);
      ++insufficient;
    } else if (failure.reason == trading::FailureReason::kSendFailed) {
      std::fprintf(stderr, "reservation_failure sequence=%" PRIu64
                           " reason=send_failed\n", failure.order_id.sequence);
      ++send_failed;
    } else {
      std::fprintf(stderr, "Unexpected reservation failure\n");
      ++unexpected;
    }
  }
  void OnCancelReport(const trading::CancelReport&) override { ++unexpected; }
  void OnTradeReport(const trading::TradeReport&) override { ++unexpected; }

  int sent = 0;
  int insufficient = 0;
  int send_failed = 0;
  int unexpected = 0;
};

// Checks both the raw counter value and the adjusted amount before printing.
bool PrintFunds(trading::TradingClient& client, double reserved) {
  trading::FundsSnapshot snapshot;
  if (!client.QueryFunds({7}, snapshot) || snapshot.funds.available != 100 ||
      snapshot.reserved != reserved ||
      snapshot.available_for_orders() != 100 - reserved) {
    return false;
  }
  std::printf("reservation account=7 reserved=%g available_for_orders=%g\n",
              snapshot.reserved, snapshot.available_for_orders());
  return true;
}

}  // namespace

int RunReservationDemo() {
  trading::SimulatedTradingAdapter adapter;
  ReservationOutput output;
  trading::TradingService service;
  trading::TradingServiceRegistry registry;
  trading::FundsTerms terms;
  terms.currency = model::Currency("CNY");
  terms.contract_multiplier = 1;
  terms.margin_rate = 0.7;
  terms.fees[0] = trading::FeeRate{};
  if (!service.BindAccount(7, adapter) || !registry.Register(3, service) ||
      !service.ConfigureFundsTerms(7, 42, trading::Side::kBuy, terms)) {
    std::fprintf(stderr, "Failed to assemble reservation scenario\n");
    return 1;
  }
  trading::TradingClientFactory factory(registry);
  auto first = factory.Create(3);
  auto second = factory.Create(3);
  if (!first || !second) {
    std::fprintf(stderr, "Failed to create reservation clients\n");
    return 1;
  }
  adapter.QueueFunds({1, trading::FundsRefreshStarted{}});
  adapter.QueueFunds({1, trading::FundsInfo{terms.currency, 100, 100, 0, 0}});
  service.Run();
  trading::SubmitOrderRequest request{{{100, 1}, 7, 2, 42,
      trading::Side::kBuy, trading::Offset::kOpen, 100, 1}};
  first->SubmitOrder(request, output);
  request.order.order_id.sequence = 2;
  request.order.strategy_id = 3;
  second->SubmitOrder(request, output);
  service.Run();
  if (!PrintFunds(*first, 70)) {
    std::fprintf(stderr, "Incorrect shared-account reservation\n");
    return 1;
  }
  request.order.order_id.sequence = 3;
  request.order.price = 20;
  adapter.SetNextSubmit({trading::SendResult::kNotSent, std::nullopt});
  first->SubmitOrder(request, output);
  service.Run();
  if (!PrintFunds(*second, 70)) {
    std::fprintf(stderr, "Send failure did not restore available funds\n");
    return 1;
  }
  request.order.order_id.sequence = 4;
  request.order.price = 30;
  second->SubmitOrder(request, output);
  service.Run();
  if (!PrintFunds(*first, 91) || output.sent != 2 || output.insufficient != 1 ||
      output.send_failed != 1 || output.unexpected != 0) {
    std::fprintf(stderr, "Incorrect reservation scenario results\n");
    return 1;
  }
  std::printf("reservation_summary sent=%d insufficient=%d send_failed=%d\n",
              output.sent, output.insufficient, output.send_failed);
  return 0;
}

}  // namespace yquant::monod
