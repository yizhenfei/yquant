#include "funds_demo.h"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "yquant/container/container.h"
#include "yquant/model/currency.h"
#include "yquant/trading/funds.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::monod {
namespace {

// Returns true after both accounts publish the requested complete refresh.
bool HasRefresh(trading::TradingClient& client, std::uint64_t id) {
  trading::FundsSnapshot first;
  trading::FundsSnapshot second;
  return client.QueryFunds({7}, first) && client.QueryFunds({8}, second) &&
         first.refresh_id == id && second.refresh_id == id;
}

// Checks every snapshot field, then writes one account's funds to stdout.
bool PrintFunds(trading::TradingClient& client, std::uint64_t account_id,
                const trading::FundsSnapshot& expected) {
  trading::FundsSnapshot snapshot;
  if (!client.QueryFunds({account_id}, snapshot) || snapshot != expected) {
    return false;
  }
  const auto& funds = snapshot.funds;
  std::printf("funds account=%" PRIu64 " ready=true refresh=%" PRIu64
              " currency=%.3s balance=%.17g available=%.17g margin=%.17g"
              " frozen_margin=%.17g\n", account_id, snapshot.refresh_id,
              funds.currency.code().data(), funds.balance, funds.available,
              funds.margin, funds.frozen_margin);
  return true;
}

}  // namespace

int RunFundsDemo() {
  trading::SimulatedTradingAdapter first;
  trading::SimulatedTradingAdapter second;
  trading::TradingService service;
  trading::TradingServiceRegistry registry;
  if (!service.BindAccount(7, first) || !service.BindAccount(8, second) ||
      !registry.Register(3, service)) {
    std::fprintf(stderr, "Failed to assemble funds scenario\n");
    return 1;
  }
  auto client = trading::TradingClientFactory(registry).Create(3);
  if (!client) {
    std::fprintf(stderr, "Failed to create funds client\n");
    return 1;
  }
  for (std::uint64_t account_id : {7U, 8U}) {
    trading::FundsSnapshot snapshot;
    if (client->QueryFunds({account_id}, snapshot)) {
      std::fprintf(stderr, "Funds available before an initial snapshot\n");
      return 1;
    }
    std::printf("funds account=%" PRIu64 " ready=false\n", account_id);
  }
  const trading::FundsInfo first_value{
      model::Currency("CNY"), 1000, 800, 200, 0};
  const trading::FundsInfo second_value{model::Currency("USD"), 0, 0, 0, 0};
  first.SetInitialState(first_value, {});
  second.SetInitialState(second_value, {});
  container::Container container;
  if (!container.Register(service) ||
      !container.Run([&] { return HasRefresh(*client, 1); }) ||
      !PrintFunds(*client, 7, {first_value, 1}) ||
      !PrintFunds(*client, 8, {second_value, 1})) {
    std::fprintf(stderr, "Failed to verify initial account funds\n");
    return 1;
  }
  std::printf("funds_summary accounts=2 initial_syncs=1\n");
  return 0;
}

}  // namespace yquant::monod
