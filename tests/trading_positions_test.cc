#include "yquant/trading/trading_client_factory.h"

#include <array>
#include <limits>
#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/model/currency.h"
#include "yquant/trading/order.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {
namespace {

// Gives fixture quantities a known zero-margin cost basis.
PositionInfo Priced(PositionInfo value) {
  if (value.today_quantity > 0) {
    value.today_cost = PositionCost{100.0 * value.today_quantity, 0};
  }
  if (value.yesterday_quantity > 0) {
    value.yesterday_cost = PositionCost{100.0 * value.yesterday_quantity, 0};
  }
  return value;
}

const PositionInfo kInitial = Priced({{42}, 3, 7, 0, 0});

// Stages startup rows through the adapter and reads only the public client.
class PositionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(service.BindAccount(7, adapter));
    TradingServiceRegistry registry;
    ASSERT_TRUE(registry.Register(3, service));
    client = TradingClientFactory(registry).Create(3);
  }
  void Begin(std::uint64_t id) {
    adapter.QueueFunds({id, FundsRefreshStarted{}});
    adapter.QueueFunds({id, FundsInfo{model::Currency("CNY"), 100, 100, 0, 0}});
    adapter.QueuePositions({id, PositionsRefreshStarted{}});
  }
  void Finish(std::uint64_t id, std::size_t count) {
    adapter.QueuePositions({id, PositionsRefreshCompleted{count}});
    adapter.QueueAccountSync({id, AccountSyncResult::kComplete});
    service.Run();
  }
  void Unavailable() {
    const std::array<PositionInfo, 1> rows{kInitial};
    PositionsSnapshot snapshot{rows, 99};
    EXPECT_FALSE(client->QueryPositions({7}, snapshot));
    EXPECT_EQ(snapshot.positions.data(), rows.data());
    EXPECT_EQ(snapshot.refresh_id, 99U);
    auto position = kInitial;
    EXPECT_FALSE(client->QueryPosition({7, kInitial.key}, position));
    EXPECT_EQ(position, kInitial);
  }
  SimulatedTradingAdapter adapter;
  TradingService service;
  std::unique_ptr<TradingClient> client;
};

TEST_F(PositionsTest,
       PartialRowsStayHiddenUntilBothSnapshotsAndBoundaryComplete) {
  Begin(1);
  adapter.QueuePositions({1, kInitial});
  service.Run();
  Unavailable();
  Finish(1, 1);
  PositionInfo position;
  ASSERT_TRUE(client->QueryPosition({7, kInitial.key}, position));
  EXPECT_EQ(position, kInitial);
  EXPECT_FALSE(client->QueryPosition({99, kInitial.key}, position));
  EXPECT_FALSE(client->QueryPosition({7, {0}}, position));
  EXPECT_EQ(position, kInitial);
}

TEST_F(PositionsTest, CompleteEmptySnapshotAndAbsentKeysReturnZero) {
  Begin(1);
  Finish(1, 0);
  PositionsSnapshot snapshot;
  ASSERT_TRUE(client->QueryPositions({7}, snapshot));
  EXPECT_TRUE(snapshot.positions.empty());
  auto position = kInitial;
  ASSERT_TRUE(client->QueryPosition({7, kInitial.key}, position));
  EXPECT_EQ(position, (PositionInfo{kInitial.key}));
}

TEST_F(PositionsTest, CompleteIdentityAndDuplicateRowsArePreserved) {
  const std::array<PositionInfo, 4> values{{
      kInitial, Priced({{42, PositionSide::kShort}, 4, 5, 0, 0}),
      Priced({{42, PositionSide::kLong, HedgeFlag::kHedge}, 1, 2, 0, 0}),
      Priced({{81}, 9, 0, 0, 0})}};
  Begin(1);
  for (auto value : values) {
    adapter.QueuePositions({1, value});
    adapter.QueuePositions({1, value});
  }
  adapter.QueuePositions({1, PositionsRefreshStarted{}});
  Finish(1, values.size());
  PositionsSnapshot snapshot;
  ASSERT_TRUE(client->QueryPositions({7}, snapshot));
  EXPECT_EQ(snapshot.positions.size(), values.size());
  for (const auto& value : values) {
    PositionInfo found;
    ASSERT_TRUE(client->QueryPosition({7, value.key}, found));
    EXPECT_EQ(found, value);
  }
}

TEST_F(PositionsTest, ConflictingRowsAndWrongCountFailTheAttempt) {
  Begin(1);
  adapter.QueuePositions({1, kInitial});
  adapter.QueuePositions({1, PositionInfo{kInitial.key, 8, 0, 0, 0}});
  Finish(1, 1);
  Unavailable();
  ASSERT_TRUE(service.RetryAccountSync(7));
  Begin(2);
  adapter.QueuePositions({2, kInitial});
  Finish(2, 2);
  Unavailable();
  ASSERT_TRUE(service.RetryAccountSync(7));
  Begin(3);
  adapter.QueuePositions({3, kInitial});
  Finish(3, 0);
  Unavailable();
  ASSERT_TRUE(service.RetryAccountSync(7));
  Begin(4);
  Finish(4, 0);
  EXPECT_TRUE(service.IsAccountReady(7));
}

TEST_F(PositionsTest, FailureAndOldResultsCannotPublishOnRetry) {
  Begin(1);
  adapter.QueuePositions({1, PositionsRefreshFailed{}});
  service.Run();
  Unavailable();
  ASSERT_TRUE(service.RetryAccountSync(7));
  Begin(2);
  adapter.QueuePositions({1, kInitial});
  adapter.QueuePositions({1, PositionsRefreshCompleted{1}});
  adapter.QueuePositions({99, PositionsRefreshStarted{}});
  Finish(2, 0);
  PositionsSnapshot snapshot;
  ASSERT_TRUE(client->QueryPositions({7}, snapshot));
  EXPECT_TRUE(snapshot.positions.empty());
  EXPECT_EQ(snapshot.refresh_id, 2U);
}

TEST_F(PositionsTest, RejectsInvalidKeysQuantitiesAndFrozenBuckets) {
  std::vector<PositionInfo> invalid;
  for (const auto key : {PositionKey{0},
                        PositionKey{42, static_cast<PositionSide>(99)},
       PositionKey{42, PositionSide::kLong, static_cast<HedgeFlag>(99)}}) {
    auto value = kInitial;
    value.key = key;
    invalid.push_back(value);
  }
  for (auto field : {&PositionInfo::today_quantity,
                     &PositionInfo::yesterday_quantity,
                     &PositionInfo::today_frozen,
                     &PositionInfo::yesterday_frozen}) {
    auto value = kInitial;
    value.*field = -1;
    invalid.push_back(value);
  }
  invalid.push_back({kInitial.key, 1, 0, 2, 0});
  invalid.push_back({kInitial.key, 0, 1, 0, 2});
  std::uint64_t id = 1;
  for (const auto& value : invalid) {
    Begin(id);
    adapter.QueuePositions({id, value});
    Finish(id++, 1);
    Unavailable();
    ASSERT_TRUE(service.RetryAccountSync(7));
  }
  Begin(id);
  constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
  adapter.QueuePositions({id, Priced({kInitial.key, kMax, kMax, 0, 0})});
  Finish(id, 1);
  PositionInfo position;
  ASSERT_TRUE(client->QueryPosition({7, kInitial.key}, position));
  EXPECT_EQ(position.today_quantity, kMax);
  EXPECT_EQ(position.available_quantity(), 2LL * kMax);
}

TEST_F(PositionsTest, LargeSnapshotIsBoundedAndLateSnapshotsDoNotOverwriteIt) {
  Begin(1);
  for (std::uint32_t i = 1; i <= 130; ++i) {
    adapter.QueuePositions({1, Priced({{i}, 1, 0, 0, 0})});
  }
  Finish(1, 130);
  Unavailable();
  service.Run();
  Unavailable();
  service.Run();
  PositionsSnapshot snapshot;
  ASSERT_TRUE(client->QueryPositions({7}, snapshot));
  EXPECT_EQ(snapshot.positions.size(), 130U);
  const auto* rows = snapshot.positions.data();
  Begin(2);
  Finish(2, 0);
  adapter.QueuePositions({1, PositionsRefreshFailed{}});
  service.Run();
  ASSERT_TRUE(client->QueryPositions({7}, snapshot));
  EXPECT_EQ(snapshot.positions.data(), rows);
  EXPECT_EQ(snapshot.positions.size(), 130U);
  EXPECT_EQ(snapshot.refresh_id, 1U);
}

}  // namespace
}  // namespace yquant::trading
