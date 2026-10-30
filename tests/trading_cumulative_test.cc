#include "yquant/trading/trading_client_factory.h"

#include <vector>

#include "gtest/gtest.h"
#include "yquant/trading/order.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {
namespace {

// Queries the service inside callbacks and retains independent event values.
class CumulativeResults final : public TradingListener {
 public:
  // TradingListener implementation.
  void OnTradeReport(const TradeReport&) override {
    ADD_FAILURE() << "Unexpected trade report";
  }
  void OnRequestFailure(const RequestFailure& failure) override {
    failures.push_back(failure);
  }
  void OnOrderReport(const OrderReport& report) override {
    OrderSnapshot current;
    const auto& info = report.snapshot.order;
    ASSERT_TRUE(client->QueryOrder(
        {info.order_id, info.account_id, info.strategy_id}, current));
    EXPECT_EQ(current.state, report.snapshot.state);
    EXPECT_EQ(current.filled_quantity, report.snapshot.filled_quantity);
    EXPECT_EQ(current.version, report.snapshot.version);
    snapshots.push_back(report.snapshot);
  }
  void OnCancelReport(const CancelReport& report) override {
    cancels.push_back(report);
  }

  TradingClient* client = nullptr;
  std::vector<OrderSnapshot> snapshots;
  std::vector<RequestFailure> failures;
  std::vector<CancelReport> cancels;
};

// Runs reports through account-bound adapters and the public client interface.
class CumulativeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(service.BindAccount(7, adapter));
    ASSERT_TRUE(service.BindAccount(8, other));
    ASSERT_TRUE(registry.Register(3, service));
    client = TradingClientFactory(registry).Create(3);
    ASSERT_NE(client, nullptr);
    results.client = client.get();
    ASSERT_EQ(client->SubmitOrder(
        {{{100, 1}, 7, 2, 42, Side::kBuy, Offset::kOpen, 100, 5}}, results),
        RequestResult::kDelivered);
    service.Run();
  }
  OrderSnapshot Snapshot() {
    OrderSnapshot snapshot;
    EXPECT_TRUE(client->QueryOrder({{100, 1}, 7, 2}, snapshot));
    return snapshot;
  }

  SimulatedTradingAdapter adapter;
  SimulatedTradingAdapter other;
  CumulativeResults results;
  TradingService service;
  TradingServiceRegistry registry;
  std::unique_ptr<TradingClient> client;
};

TEST_F(CumulativeTest,
       MonotonicCumulativeQuantityProducesPartialAndFullStates) {
  const auto accepted = Snapshot();
  adapter.Emit({{100, 1}, AdapterOrderState::kAccepted, 2});
  EXPECT_EQ(Snapshot().state, OrderState::kPartiallyFilled);
  EXPECT_EQ(Snapshot().filled_quantity, 2);
  EXPECT_EQ(Snapshot().version, accepted.version + 1);
  for (int quantity : {2, 1, 0, -1, 6}) {
    adapter.Emit({{100, 1}, AdapterOrderState::kAccepted, quantity});
  }
  other.Emit({{100, 1}, AdapterOrderState::kAccepted, 5});
  adapter.Emit({{100, 9}, AdapterOrderState::kAccepted, 5});
  adapter.Emit({{100, 1}, static_cast<AdapterOrderState>(99), 5});
  adapter.Emit({{100, 1}, AdapterOrderState::kRejected, 0});
  EXPECT_EQ(Snapshot().filled_quantity, 2);
  EXPECT_EQ(results.snapshots.size(), 2U);
  adapter.Emit({{100, 1}, AdapterOrderState::kAccepted, 5});
  const auto filled = Snapshot();
  EXPECT_EQ(filled.state, OrderState::kFilled);
  EXPECT_EQ(filled.filled_quantity, 5);
  EXPECT_EQ(filled.version, accepted.version + 2);
  adapter.Emit({{100, 1}, AdapterOrderState::kAccepted, 0});
  EXPECT_EQ(Snapshot().version, filled.version);
  EXPECT_EQ(results.snapshots.front().filled_quantity, 0);
}

TEST_F(CumulativeTest, PartialOrderCanCancelAndLateFillsPreserveCancelFact) {
  adapter.Emit({{100, 1}, AdapterOrderState::kAccepted, 2});
  ASSERT_EQ(client->CancelOrder({{100, 1}, 7, 2, 1}, results),
            RequestResult::kDelivered);
  service.Run();
  ASSERT_EQ(results.cancels.size(), 1U);
  EXPECT_EQ(results.cancels[0].result, CancelResult::kCanceled);
  EXPECT_EQ(Snapshot().state, OrderState::kCanceled);
  EXPECT_EQ(Snapshot().filled_quantity, 2);
  adapter.Emit({{100, 1}, AdapterOrderState::kAccepted, 3});
  EXPECT_EQ(Snapshot().state, OrderState::kCanceled);
  EXPECT_EQ(Snapshot().filled_quantity, 3);
  adapter.Emit({{100, 1}, AdapterOrderState::kAccepted, 5});
  EXPECT_EQ(Snapshot().state, OrderState::kFilled);
}

TEST_F(CumulativeTest, FullFillWhileCancelPendingCannotBeRolledBack) {
  adapter.SetNextCancel({SendResult::kSent, std::nullopt});
  ASSERT_EQ(client->CancelOrder({{100, 1}, 7, 2, 1}, results),
            RequestResult::kDelivered);
  adapter.Emit({{100, 1}, AdapterOrderState::kAccepted, 5});
  const auto version = Snapshot().version;
  adapter.EmitCancel({{100, 1}, 1, CancelResult::kCanceled});
  ASSERT_EQ(results.cancels.size(), 1U);
  EXPECT_EQ(results.cancels[0].snapshot.state, OrderState::kFilled);
  EXPECT_EQ(Snapshot().version, version);
  EXPECT_EQ(client->CancelOrder({{100, 1}, 7, 2, 2}, results),
            RequestResult::kDelivered);
  service.Run();
  ASSERT_EQ(results.failures.size(), 1U);
  EXPECT_EQ(results.failures[0].reason, FailureReason::kOrderInactive);
}

TEST_F(CumulativeTest,
       FillBeforeAcceptanceEstablishesStateAndRejectsBadReports) {
  adapter.SetNextSubmit({SendResult::kSent, std::nullopt});
  ASSERT_EQ(client->SubmitOrder(
      {{{100, 2}, 7, 2, 42, Side::kBuy, Offset::kOpen, 100, 5}}, results),
      RequestResult::kDelivered);
  OrderSnapshot snapshot;
  adapter.Emit({{100, 2}, AdapterOrderState::kRejected, 1});
  ASSERT_TRUE(client->QueryOrder({{100, 2}, 7, 2}, snapshot));
  EXPECT_EQ(snapshot.state, OrderState::kSent);
  adapter.Emit({{100, 2}, AdapterOrderState::kAccepted, 2});
  adapter.Emit({{100, 2}, AdapterOrderState::kAccepted, 0});
  ASSERT_TRUE(client->QueryOrder({{100, 2}, 7, 2}, snapshot));
  EXPECT_EQ(snapshot.state, OrderState::kPartiallyFilled);
  EXPECT_EQ(snapshot.filled_quantity, 2);
}

TEST_F(CumulativeTest,
       RejectionCannotAcquireFillsAndSendFailureRemainsQueryable) {
  adapter.SetNextSubmit({SendResult::kSent, AdapterOrderState::kRejected});
  ASSERT_EQ(client->SubmitOrder(
      {{{100, 2}, 7, 2, 42, Side::kBuy, Offset::kOpen, 100, 5}}, results),
      RequestResult::kDelivered);
  service.Run();
  OrderSnapshot snapshot;
  ASSERT_TRUE(client->QueryOrder({{100, 2}, 7, 2}, snapshot));
  const auto version = snapshot.version;
  adapter.Emit({{100, 2}, AdapterOrderState::kRejected, 0});
  adapter.Emit({{100, 2}, AdapterOrderState::kAccepted, 5});
  ASSERT_TRUE(client->QueryOrder({{100, 2}, 7, 2}, snapshot));
  EXPECT_EQ(snapshot.state, OrderState::kRejected);
  EXPECT_EQ(snapshot.version, version);
  EXPECT_EQ(snapshot.filled_quantity, 0);
  adapter.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  ASSERT_EQ(client->SubmitOrder(
      {{{100, 3}, 7, 2, 42, Side::kBuy, Offset::kOpen, 100, 5}}, results),
      RequestResult::kDelivered);
  ASSERT_TRUE(client->QueryOrder({{100, 3}, 7, 2}, snapshot));
  EXPECT_EQ(snapshot.state, OrderState::kSendFailed);
  adapter.Emit({{100, 3}, AdapterOrderState::kAccepted, 5});
  service.Run();
  ASSERT_TRUE(client->QueryOrder({{100, 3}, 7, 2}, snapshot));
  EXPECT_EQ(snapshot.state, OrderState::kFilled);
  ASSERT_EQ(results.failures.size(), 1U);
  ASSERT_TRUE(results.failures[0].snapshot);
  EXPECT_EQ(results.failures[0].snapshot->state, OrderState::kSendFailed);
}

}  // namespace
}  // namespace yquant::trading
