#include "positions_demo.h"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>

#include "yquant/container/container.h"
#include "yquant/model/currency.h"
#include "yquant/trading/funds.h"
#include "yquant/trading/order.h"
#include "yquant/trading/position.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::monod {
namespace {

// Names the hedge category in a validated position snapshot.
const char* HedgeName(trading::HedgeFlag hedge) {
  switch (hedge) {
    case trading::HedgeFlag::kSpeculation:
      return "speculation";
    case trading::HedgeFlag::kArbitrage:
      return "arbitrage";
    case trading::HedgeFlag::kHedge:
      return "hedge";
  }
  return "unknown";
}

// Waits for this first-account refresh while the second account keeps round 1.
bool HasRefresh(trading::TradingClient& client, std::uint64_t id) {
  trading::PositionsSnapshot first;
  trading::PositionsSnapshot second;
  return client.QueryPositions({7}, first) &&
         client.QueryPositions({8}, second) &&
         first.refresh_id == id && second.refresh_id == 1;
}

// Verifies both query forms, then prints every published position field.
bool PrintPositions(trading::TradingClient& client, std::uint64_t account,
                    std::uint64_t id,
                    std::span<const trading::PositionInfo> expected) {
  trading::PositionsSnapshot snapshot;
  if (!client.QueryPositions({account}, snapshot) ||
      snapshot.refresh_id != id ||
      snapshot.positions.size() != expected.size()) {
    return false;
  }
  std::printf("positions account=%" PRIu64 " ready=true refresh=%" PRIu64
              " count=%zu\n", account, id, snapshot.positions.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto& row = snapshot.positions[index];
    trading::PositionInfo single;
    if (row != expected[index] ||
        !client.QueryPosition({account, row.key}, single) || single != row) {
      return false;
    }
    std::printf("position account=%" PRIu64 " instrument=%" PRIu32
                " side=%s hedge=%s today=%d yesterday=%d today_frozen=%d"
                " yesterday_frozen=%d\n", account, row.key.instrument_id,
                row.key.side == trading::PositionSide::kLong ? "long" : "short",
                HedgeName(row.key.hedge), row.today_quantity,
                row.yesterday_quantity, row.today_frozen, row.yesterday_frozen);
  }
  return true;
}

}  // namespace

int RunPositionsDemo() {
  trading::SimulatedTradingAdapter first;
  trading::SimulatedTradingAdapter second;
  trading::TradingService service;
  trading::TradingServiceRegistry registry;
  if (!service.BindAccount(7, first) || !service.BindAccount(8, second) ||
      !registry.Register(3, service)) {
    std::fprintf(stderr, "Failed to assemble positions scenario\n");
    return 1;
  }
  auto client = trading::TradingClientFactory(registry).Create(3);
  if (!client) {
    std::fprintf(stderr, "Failed to create positions client\n");
    return 1;
  }
  for (std::uint64_t account : {7U, 8U}) {
    trading::PositionsSnapshot snapshot;
    if (client->QueryPositions({account}, snapshot)) {
      std::fprintf(stderr, "Positions available before an initial snapshot\n");
      return 1;
    }
    std::printf("positions account=%" PRIu64 " ready=false\n", account);
  }
  const trading::PositionKey key{
      42, trading::PositionSide::kLong, trading::HedgeFlag::kSpeculation};
  const std::array<trading::PositionInfo, 2> initial{{
      {key, 3, 7, 0, 0, trading::PositionCost{300, 60},
       trading::PositionCost{700, 140}},
      {{81, trading::PositionSide::kShort, trading::HedgeFlag::kArbitrage},
       2, 0, 0, 0, trading::PositionCost{200, 0}},
  }};
  const trading::FundsInfo funds{model::Currency("CNY"), 1000, 800, 200, 0};
  first.SetInitialState(funds, initial);
  second.SetInitialState(funds, {});
  container::Container container;
  if (!container.Register(service) ||
      !container.Run([&] { return HasRefresh(*client, 1); }) ||
      !PrintPositions(*client, 7, 1, initial) ||
      !PrintPositions(*client, 8, 1, {})) {
    std::fprintf(stderr, "Failed to verify initial positions\n");
    return 1;
  }
  trading::PositionInfo zero;
  if (!client->QueryPosition({8, key}, zero) ||
      zero != trading::PositionInfo{key}) {
    std::fprintf(stderr, "Failed to verify empty account positions\n");
    return 1;
  }
  std::printf("zero_position account=8 instrument=42 today=0 yesterday=0\n"
              "positions_summary accounts=2 initial_syncs=1\n");
  return 0;
}

}  // namespace yquant::monod
