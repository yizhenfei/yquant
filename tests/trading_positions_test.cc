#include "yquant/trading/trading_client_factory.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/model/currency.h"
#include "yquant/trading/funds.h"
#include "yquant/trading/position.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {
namespace {

const PositionKey kLong{42, PositionSide::kLong, HedgeFlag::kSpeculation};
const PositionKey kShort{42, PositionSide::kShort, HedgeFlag::kSpeculation};
const PositionKey kHedge{42, PositionSide::kLong, HedgeFlag::kHedge};
const PositionInfo kInitial{kLong, 3, 7, 1, 2};
const PositionInfo kUpdated{kLong, 5, 2, 2, 1};

// Reads position state through two factory-created clients sharing a service.
class PositionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(service.BindAccount(7, first));
    ASSERT_TRUE(service.BindAccount(8, second));
    TradingServiceRegistry registry;
    ASSERT_TRUE(registry.Register(3, service));
    client = TradingClientFactory(registry).Create(3);
    observer = TradingClientFactory(registry).Create(3);
    ASSERT_NE(client, nullptr);
    ASSERT_NE(observer, nullptr);
  }
  void Begin(SimulatedTradingAdapter& adapter, std::uint64_t id) {
    adapter.QueuePositions({id, PositionsRefreshStarted{}});
  }
  void Row(SimulatedTradingAdapter& adapter, std::uint64_t id,
           const PositionInfo& value) {
    adapter.QueuePositions({id, value});
  }
  void Finish(SimulatedTradingAdapter& adapter, std::uint64_t id,
              std::size_t count) {
    adapter.QueuePositions({id, PositionsRefreshCompleted{count}});
  }
  void Publish(SimulatedTradingAdapter& adapter, std::uint64_t id,
               std::initializer_list<PositionInfo> rows) {
    Begin(adapter, id);
    for (const auto& value : rows) {
      Row(adapter, id, value);
    }
    Finish(adapter, id, rows.size());
    service.Run();
  }
  PositionInfo Position(PositionKey key = kLong, std::uint64_t account = 7) {
    PositionInfo output;
    EXPECT_TRUE(client->QueryPosition({account, key}, output));
    return output;
  }
  PositionsSnapshot Snapshot(std::uint64_t account = 7) {
    PositionsSnapshot output;
    EXPECT_TRUE(client->QueryPositions({account}, output));
    return output;
  }
  void ExpectUnavailable(std::uint64_t account = 7) {
    const std::array<PositionInfo, 1> sentinel_rows{kInitial};
    PositionsSnapshot snapshot{sentinel_rows, 999};
    EXPECT_FALSE(client->QueryPositions({account}, snapshot));
    EXPECT_EQ(snapshot.positions.data(), sentinel_rows.data());
    EXPECT_EQ(snapshot.positions.size(), 1U);
    EXPECT_EQ(snapshot.refresh_id, 999U);
    auto output = kInitial;
    EXPECT_FALSE(client->QueryPosition({account, kLong}, output));
    EXPECT_EQ(output, kInitial);
  }

  SimulatedTradingAdapter first;
  SimulatedTradingAdapter second;
  TradingService service;
  std::unique_ptr<TradingClient> client;
  std::unique_ptr<TradingClient> observer;
};

TEST_F(PositionsTest, UnknownAndUnfinishedAccountsLeaveBothOutputsUntouched) {
  ExpectUnavailable();
  ExpectUnavailable(99);
  Begin(first, 1);
  Row(first, 1, kInitial);
  service.Run();
  ExpectUnavailable();
  Finish(first, 1, 1);
  service.Run();
  EXPECT_EQ(Position(), kInitial);
}

TEST_F(PositionsTest, CompleteEmptyAccountIsAvailableAndMissingKeysMeanZero) {
  Publish(first, 1, {});
  EXPECT_TRUE(Snapshot().positions.empty());
  EXPECT_EQ(Snapshot().refresh_id, 1U);
  EXPECT_EQ(Position(), (PositionInfo{kLong}));
  PositionsSnapshot shared;
  EXPECT_TRUE(observer->QueryPositions({7}, shared));
  EXPECT_TRUE(shared.positions.empty());
  ExpectUnavailable(8);
}

TEST_F(PositionsTest, KeysDistinguishInstrumentDirectionAndHedgeCategory) {
  const PositionInfo short_position{kShort, 2, 4, 0, 1};
  const PositionInfo hedge_position{kHedge, 1, 2, 0, 0};
  const PositionKey other{81, PositionSide::kLong, HedgeFlag::kArbitrage};
  const PositionInfo other_position{other, 8, 0, 2, 0};
  Publish(first, 1, {kInitial, short_position, hedge_position, other_position});
  EXPECT_EQ(Position(), kInitial);
  EXPECT_EQ(Position(kShort), short_position);
  EXPECT_EQ(Position(kHedge), hedge_position);
  EXPECT_EQ(Position(other), other_position);
  EXPECT_EQ(Snapshot().positions.size(), 4U);
  PositionInfo shared;
  EXPECT_TRUE(observer->QueryPosition({7, kHedge}, shared));
  EXPECT_EQ(shared, hedge_position);
}

TEST_F(PositionsTest,
       ReplacesTheWholeBookAndRemovesRowsMissingFromNewSnapshot) {
  const PositionInfo short_position{kShort, 2, 4, 0, 1};
  Publish(first, 1, {kInitial, short_position});
  const auto old = Snapshot();
  const std::vector<PositionInfo> retained(old.positions.begin(),
                                           old.positions.end());
  Begin(first, 2);
  Row(first, 2, kUpdated);
  Row(first, 2, {kHedge, 1, 1, 0, 0});
  service.Run();
  EXPECT_EQ(Position(), kInitial);
  EXPECT_EQ(Snapshot().refresh_id, 1U);
  EXPECT_EQ(old.positions[0], kInitial);
  Finish(first, 2, 2);
  service.Run();
  EXPECT_EQ(Position(), kUpdated);
  EXPECT_EQ(Position(kShort), (PositionInfo{kShort}));
  EXPECT_EQ(Snapshot().positions.size(), 2U);
  EXPECT_EQ(Snapshot().refresh_id, 2U);
  EXPECT_EQ(retained[0], kInitial);
  Publish(first, 3, {});
  EXPECT_TRUE(Snapshot().positions.empty());
  EXPECT_EQ(Position(), (PositionInfo{kLong}));
}

TEST_F(PositionsTest, SameKeysAndRefreshIdsRemainIndependentAcrossAccounts) {
  const PositionInfo other{kLong, 20, 30, 4, 5};
  Publish(first, 1, {kInitial});
  Publish(second, 1, {other});
  Publish(first, 2, {});
  EXPECT_EQ(Position(), (PositionInfo{kLong}));
  EXPECT_EQ(Position(kLong, 8), other);
  EXPECT_EQ(Snapshot(8).refresh_id, 1U);
}

TEST_F(PositionsTest, IdenticalDuplicateRowsAreIdempotent) {
  Begin(first, 1);
  Row(first, 1, kInitial);
  Begin(first, 1);
  Row(first, 1, kInitial);
  Finish(first, 1, 1);
  service.Run();
  EXPECT_EQ(Position(), kInitial);
  EXPECT_EQ(Snapshot().positions.size(), 1U);
  Finish(first, 1, 0);
  Row(first, 1, kUpdated);
  service.Run();
  EXPECT_EQ(Position(), kInitial);
}

TEST_F(PositionsTest, MissingRowsAndConflictingDuplicatesCannotPublish) {
  Publish(first, 1, {kInitial});
  Begin(first, 2);
  Row(first, 2, kUpdated);
  Finish(first, 2, 2);
  service.Run();
  ExpectUnavailable();
  Begin(first, 3);
  Row(first, 3, kInitial);
  Row(first, 3, kUpdated);
  Finish(first, 3, 1);
  service.Run();
  ExpectUnavailable();
  Publish(first, 4, {kUpdated});
  EXPECT_EQ(Position(), kUpdated);
}

TEST_F(PositionsTest, FailureOrInterruptionClosesTheRoundWithoutPublishing) {
  Begin(first, 1);
  first.QueuePositions({1, PositionsRefreshFailed{}});
  ::testing::internal::CaptureStderr();
  service.Run();
  const auto diagnostic = ::testing::internal::GetCapturedStderr();
  EXPECT_NE(diagnostic.find("positions_refresh_failed account=7 refresh=1"),
            std::string::npos);
  ExpectUnavailable();
  Publish(first, 2, {kInitial});
  Begin(first, 3);
  Row(first, 3, kUpdated);
  first.QueuePositions({3, PositionsRefreshFailed{}});
  Finish(first, 3, 1);
  service.Run();
  ExpectUnavailable();
  Begin(first, 4);
  service.Run();
  ExpectUnavailable();
  Finish(first, 4, 0);
  service.Run();
  EXPECT_TRUE(Snapshot().positions.empty());
}

TEST_F(PositionsTest, NewRoundSupersedesPartialRowsAndIgnoresOldResults) {
  Publish(first, 1, {kInitial});
  Begin(first, 2);
  Row(first, 2, {kShort, 9, 0, 0, 0});
  Begin(first, 3);
  Row(first, 2, kInitial);
  Finish(first, 2, 2);
  first.QueuePositions({2, PositionsRefreshFailed{}});
  Row(first, 3, kUpdated);
  Finish(first, 3, 1);
  Begin(first, 3);
  Finish(first, 3, 0);
  service.Run();
  EXPECT_EQ(Position(), kUpdated);
  EXPECT_EQ(Position(kShort), (PositionInfo{kShort}));
  EXPECT_EQ(Snapshot().refresh_id, 3U);
}

TEST_F(PositionsTest, UnstartedZeroAndStaleRoundsDoNotChangePublishedState) {
  Row(first, 99, kInitial);
  Finish(first, 99, 1);
  Begin(first, 0);
  Row(first, 0, kInitial);
  Finish(first, 0, 1);
  service.Run();
  ExpectUnavailable();
  constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
  Publish(first, kMax, {kInitial});
  Begin(first, 1);
  Row(first, 1, kUpdated);
  Finish(first, 1, 1);
  service.Run();
  EXPECT_EQ(Position(), kInitial);
  EXPECT_EQ(Snapshot().refresh_id, kMax);
}

TEST_F(PositionsTest, RejectsInvalidKeysQuantitiesAndFrozenBuckets) {
  std::vector<PositionInfo> invalid;
  for (auto key : {PositionKey{0, PositionSide::kLong, HedgeFlag::kSpeculation},
                   PositionKey{42, static_cast<PositionSide>(99),
                               HedgeFlag::kSpeculation},
                   PositionKey{42, PositionSide::kLong,
                               static_cast<HedgeFlag>(99)}}) {
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
  auto value = kInitial;
  value.today_frozen = 4;
  invalid.push_back(value);
  value = kInitial;
  value.yesterday_frozen = 8;
  invalid.push_back(value);
  std::uint64_t id = 0;
  for (const auto& bad : invalid) {
    Publish(first, ++id, {bad});
    ExpectUnavailable();
    Row(first, id, kInitial);
    Finish(first, id, 1);
    service.Run();
    ExpectUnavailable();
  }
  Publish(first, ++id, {kInitial});
  EXPECT_EQ(Position(), kInitial);
  for (const auto& bad : invalid) {
    if (bad.key == kLong) {
      continue;
    }
    auto output = kInitial;
    EXPECT_FALSE(client->QueryPosition({7, bad.key}, output));
    EXPECT_EQ(output, kInitial);
  }
}

TEST_F(PositionsTest,
       QueuedRowsOwnValuesAndBatchingDoesNotStarveOtherAccounts) {
  Begin(first, 1);
  AdapterPositionsReport report{1, kInitial};
  first.QueuePositions(report);
  report.event = kUpdated;
  Finish(first, 1, 1);
  Publish(second, 1, {kUpdated});
  EXPECT_EQ(Position(), kInitial);
  for (std::uint64_t id = 2; id <= 66; ++id) {
    Begin(first, id);
    Finish(first, id, 0);
  }
  Publish(second, 2, {});
  EXPECT_EQ(Snapshot().refresh_id, 33U);
  EXPECT_TRUE(Snapshot(8).positions.empty());
  service.Run();
  EXPECT_EQ(Snapshot().refresh_id, 65U);
  service.Run();
  EXPECT_EQ(Snapshot().refresh_id, 66U);
}

TEST_F(PositionsTest,
       LargeSnapshotsStayHiddenUntilCompleteAndReuseBothBuffers) {
  Publish(first, 1, {kInitial});
  const auto old = Snapshot();
  Begin(first, 2);
  for (std::uint32_t instrument = 100; instrument < 230; ++instrument) {
    Row(first, 2, {{instrument}, 2, 3, 1, 2});
  }
  Finish(first, 2, 130);
  service.Run();
  EXPECT_EQ(Position(), kInitial);
  EXPECT_EQ(old.positions[0], kInitial);
  service.Run();
  EXPECT_EQ(Snapshot().refresh_id, 1U);
  service.Run();
  ASSERT_EQ(Snapshot().positions.size(), 130U);
  EXPECT_EQ(Snapshot().refresh_id, 2U);
  for (std::uint32_t instrument = 100; instrument < 230; ++instrument) {
    EXPECT_EQ(Position({instrument}), (PositionInfo{{instrument}, 2, 3, 1, 2}));
  }
  Publish(first, 3, {kUpdated});
  EXPECT_EQ(Position(), kUpdated);
  EXPECT_EQ(Position({100}), (PositionInfo{{100}}));
  EXPECT_EQ(Snapshot().positions.size(), 1U);
}

TEST_F(PositionsTest, BothQuantityBucketsSupportTheirFullRange) {
  constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
  const PositionInfo maximum{kLong, kMax, kMax, kMax, kMax};
  Publish(first, 1, {maximum});
  EXPECT_EQ(Position(), maximum);
  Publish(first, 2, {{kLong}});
  EXPECT_EQ(Position(), (PositionInfo{kLong}));
}

TEST_F(PositionsTest, FundsAndPositionsHaveIndependentRefreshAvailability) {
  first.QueueFunds({1, FundsRefreshStarted{}});
  const FundsInfo funds{model::Currency("CNY"), 1000, 700, 200, 100};
  first.QueueFunds({1, funds});
  Publish(first, 1, {kInitial});
  Begin(first, 2);
  first.QueuePositions({2, PositionsRefreshFailed{}});
  service.Run();
  ExpectUnavailable();
  FundsSnapshot snapshot;
  ASSERT_TRUE(client->QueryFunds({7}, snapshot));
  EXPECT_EQ(snapshot, (FundsSnapshot{funds, 1}));
  Publish(first, 3, {kUpdated});
  first.QueueFunds({2, FundsRefreshStarted{}});
  first.QueueFunds({2, FundsRefreshFailed{}});
  service.Run();
  EXPECT_FALSE(client->QueryFunds({7}, snapshot));
  EXPECT_EQ(Position(), kUpdated);
}

TEST_F(PositionsTest, SmallerCountAndConflictingFrozenValuesAlsoFailTheRound) {
  Begin(first, 1);
  Row(first, 1, kInitial);
  Finish(first, 1, 0);
  service.Run();
  ExpectUnavailable();
  Begin(first, 2);
  Row(first, 2, kInitial);
  auto conflict = kInitial;
  ++conflict.today_frozen;
  Row(first, 2, conflict);
  Finish(first, 2, 1);
  service.Run();
  ExpectUnavailable();
}

}  // namespace
}  // namespace yquant::trading
