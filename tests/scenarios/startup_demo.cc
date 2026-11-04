#include "startup_demo.h"

#include <array>
#include <cstdio>

#include "yquant/model/currency.h"
#include "yquant/trading/funds.h"
#include "yquant/trading/position.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::verification {

int RunStartupDemo() {
  trading::SimulatedTradingAdapter adapter;
  trading::TradingService service;
  trading::TradingServiceRegistry registry;
  if (!service.BindAccount(7, adapter) || !registry.Register(3, service)) {
    std::fprintf(stderr, "Failed to assemble startup scenario\n");
    return 1;
  }
  auto client = trading::TradingClientFactory(registry).Create(3);
  if (!client) {
    std::fprintf(stderr, "Failed to create startup client\n");
    return 1;
  }
  const trading::FundsInfo funds{model::Currency("CNY"), 100, 90, 10, 0};
  const std::array<trading::PositionInfo, 1> positions{{
      {{42}, 1, 0, 0, 0, trading::PositionCost{100, 10}}}};
  adapter.SetInitialState(funds, positions);
  // A failed request result precedes the queued successful baseline. Its late
  // data must not reopen the failed attempt; an explicit retry is required.
  adapter.QueueAccountSync({1, trading::AccountSyncResult::kFailed});
  service.Run();
  trading::FundsSnapshot cash;
  trading::PositionsSnapshot holdings;
  if (service.IsAccountReady(7) || client->QueryFunds({7}, cash) ||
      client->QueryPositions({7}, holdings) || !service.RetryAccountSync(7)) {
    std::fprintf(stderr, "Failed initialization became available\n");
    return 1;
  }
  std::printf("startup account=7 ready=false attempts=1\n");
  service.Run();
  if (!service.IsAccountReady(7) || !client->QueryFunds({7}, cash) ||
      !client->QueryPositions({7}, holdings) || cash.funds != funds ||
      holdings.positions.size() != 1 || holdings.positions[0] != positions[0]) {
    std::fprintf(stderr, "Startup retry did not publish the baseline\n");
    return 1;
  }
  adapter.QueueFunds({3, trading::FundsRefreshStarted{}});
  adapter.QueueFunds(
      {3, trading::FundsInfo{model::Currency("CNY"), 0, 0, 0, 0}});
  service.Run();
  if (!client->QueryFunds({7}, cash) || cash.funds != funds ||
      adapter.sync_requests() != 2) {
    std::fprintf(stderr, "Late snapshot changed the initialized account\n");
    return 1;
  }
  std::printf("startup account=7 ready=true attempts=2 funds=90 positions=1\n"
              "startup_summary late_snapshot_ignored=true\n");
  return 0;
}

}  // namespace yquant::verification
