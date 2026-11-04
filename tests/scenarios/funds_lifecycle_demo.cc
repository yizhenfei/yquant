#include "funds_lifecycle_demo.h"

#include <cstdio>
#include <initializer_list>

#include "yquant/model/currency.h"
#include "yquant/trading/funds_terms.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::verification {
namespace {

// Records unexpected outcomes while account values are checked through clients.
class FundsResults final : public trading::TradingListener {
 public:
  // trading::TradingListener implementation.
  void OnRequestFailure(const trading::RequestFailure&) override {
    failed = true;
  }
  void OnOrderReport(const trading::OrderReport&) override {}
  void OnCancelReport(const trading::CancelReport& report) override {
    failed |= report.result != trading::CancelResult::kCanceled;
  }
  void OnTradeReport(const trading::TradeReport&) override { ++trades; }
  bool failed = false;
  int trades = 0;
};

// Prints only a successfully queried and verified account state.
bool PrintFunds(trading::TradingClient& client, const char* stage,
                double available, double reserved, double margin, double fee,
                double profit) {
  trading::FundsSnapshot funds;
  if (!client.QueryFunds({7}, funds) ||
      funds.available_for_orders() != available ||
      funds.reserved != reserved || funds.funds.margin != margin ||
      funds.commission != fee || funds.realized_pnl != profit) return false;
  std::printf("funds_lifecycle stage=%s available=%g reserved=%g margin=%g"
              " commission=%g realized_pnl=%g\n",
              stage, available, reserved, margin, fee, profit);
  return true;
}

// Runs the complete scenario and returns false at the first failed check.
bool VerifyFundsLifecycle() {
  trading::SimulatedTradingAdapter adapter;
  FundsResults results;
  trading::TradingService service;
  trading::TradingServiceRegistry registry;
  if (!service.BindAccount(7, adapter) || !registry.Register(3, service)) {
    return false;
  }
  trading::FundsTerms terms;
  terms.currency = model::Currency("CNY");
  terms.contract_multiplier = 1;
  terms.margin_rate = 0.1;
  for (auto& fee : terms.fees) fee = trading::FeeRate{1, 0};
  for (auto side : {trading::Side::kBuy, trading::Side::kSell}) {
    if (!service.ConfigureFundsTerms(7, 42, side, terms)) return false;
  }
  auto client = trading::TradingClientFactory(registry).Create(3);
  if (!client) return false;
  adapter.SetInitialState({terms.currency, 1000, 1000, 0, 0}, {});
  service.Run();
  trading::SubmitOrderRequest request{{{100, 1}, 7, 2, 42, trading::Side::kBuy,
                                      trading::Offset::kOpen, 100, 2}};
  client->SubmitOrder(request, results);
  service.Run();
  if (!PrintFunds(*client, "submitted", 978, 22, 0, 0, 0) ||
      !adapter.Fill({{100, 1}, {"sim", "day", "open"}, 90, 1})) return false;
  service.Run();
  if (!PrintFunds(*client, "partial", 979, 11, 9, 1, 0)) return false;
  client->CancelOrder({{100, 1}, 7, 2, 1}, results);
  service.Run();
  if (!PrintFunds(*client, "canceled", 990, 0, 9, 1, 0)) return false;
  request.order = {{100, 2}, 7, 2, 42, trading::Side::kSell,
                    trading::Offset::kCloseToday, 110, 1};
  client->SubmitOrder(request, results);
  service.Run();
  if (!adapter.Fill({{100, 2}, {"sim", "day", "close"}, 110, 1})) return false;
  service.Run();
  if (results.failed || results.trades != 2 ||
      !PrintFunds(*client, "closed", 1018, 0, 0, 2, 20)) {
    return false;
  }
  std::printf("funds_lifecycle_summary trades=2\n");
  return true;
}

}  // namespace

int RunFundsLifecycleDemo() {
  if (!VerifyFundsLifecycle()) {
    std::fprintf(stderr, "Failed to verify funds lifecycle\n");
    return 1;
  }
  return 0;
}

}  // namespace yquant::verification
