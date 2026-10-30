#include "yquant/trading/trading_client_factory.h"

#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/trading/order.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trade.h"
#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {
namespace {

// Retains each event and queries the order from inside its trade callback.
class TradeResults final : public TradingListener {
 public:
  // TradingListener implementation.
  void OnRequestFailure(const RequestFailure& failure) override {
    failures.push_back(failure);
  }
  void OnOrderReport(const OrderReport& report) override {
    orders.push_back(report);
  }
  void OnCancelReport(const CancelReport& report) override {
    cancels.push_back(report);
  }
  void OnTradeReport(const TradeReport& report) override {
    EXPECT_EQ(std::this_thread::get_id(), thread);
    OrderSnapshot current;
    const auto& order = report.snapshot.order;
    ASSERT_TRUE(client->QueryOrder(
        {order.order_id, order.account_id, order.strategy_id}, current));
    EXPECT_EQ(current.order, order);
    EXPECT_GE(current.version, report.snapshot.version);
    EXPECT_GE(current.filled_quantity, report.snapshot.filled_quantity);
    if (current.version == report.snapshot.version) {
      EXPECT_EQ(current.state, report.snapshot.state);
      EXPECT_EQ(current.filled_quantity, report.snapshot.filled_quantity);
    }
    trades.push_back(report);
    if (action) {
      action(report);
    }
  }

  TradingClient* client = nullptr;
  std::thread::id thread = std::this_thread::get_id();
  std::vector<OrderReport> orders;
  std::vector<TradeReport> trades;
  std::vector<CancelReport> cancels;
  std::vector<RequestFailure> failures;
  std::function<void(const TradeReport&)> action;
};

// Tests observable state through two account adapters and a factory-made client.
class TradeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(service.BindAccount(7, first));
    ASSERT_TRUE(service.BindAccount(8, second));
    ASSERT_TRUE(registry.Register(3, service));
    client = TradingClientFactory(registry).Create(3);
    ASSERT_NE(client, nullptr);
    results.client = client.get();
    foreign.client = client.get();
    Submit({100, 1}, 7, results);
  }
  void Submit(OrderId id, std::uint64_t account, TradeResults& listener,
              std::int32_t quantity = 5) {
    EXPECT_EQ(client->SubmitOrder(
        {{id, account, 2, 42, Side::kBuy, Offset::kOpen, 100, quantity}},
        listener), RequestResult::kDelivered);
  }
  TradeInfo Trade(const char* value, std::int32_t quantity = 2) {
    return {{100, 1}, {"sim", "day-1", value}, 100.25, quantity};
  }
  OrderSnapshot Snapshot(OrderId id = {100, 1}, std::uint64_t account = 7) {
    OrderSnapshot snapshot;
    EXPECT_TRUE(client->QueryOrder({id, account, 2}, snapshot));
    return snapshot;
  }

  SimulatedTradingAdapter first;
  SimulatedTradingAdapter second;
  TradeResults results;
  TradeResults foreign;
  TradingService service;
  TradingServiceRegistry registry;
  std::unique_ptr<TradingClient> client;
};

TEST_F(TradeTest, DeliversPartialAndFullTradesThroughSubsequentRun) {
  service.Run();
  auto trade = Trade("1");
  const auto original = trade;
  ASSERT_TRUE(first.Fill(trade));
  EXPECT_TRUE(results.trades.empty());
  EXPECT_EQ(Snapshot().filled_quantity, 0);
  trade.price = 1;
  trade.trade_id.value = "changed";
  service.Run();
  ASSERT_EQ(results.trades.size(), 1U);
  EXPECT_EQ(results.trades[0].trade, original);
  EXPECT_EQ(Snapshot().state, OrderState::kPartiallyFilled);
  EXPECT_EQ(Snapshot().filled_quantity, 2);
  EXPECT_EQ(results.orders.size(), 1U);
  ASSERT_TRUE(first.Fill(Trade("2", 3)));
  service.Run();
  EXPECT_EQ(Snapshot().state, OrderState::kFilled);
  EXPECT_EQ(Snapshot().filled_quantity, 5);
  EXPECT_EQ(results.trades[0].snapshot.filled_quantity, 2);
  EXPECT_FALSE(first.Fill(Trade("3", 1)));
  EXPECT_EQ(client->CancelOrder({{100, 1}, 7, 2, 1}, results),
            RequestResult::kDelivered);
  service.Run();
  ASSERT_EQ(results.failures.size(), 1U);
  EXPECT_EQ(results.failures[0].reason, FailureReason::kOrderInactive);
}

TEST_F(TradeTest, DeduplicatesFullIdentityAndDiagnosesConflictingContent) {
  service.Run();
  const auto trade = Trade("1");
  first.EmitTrade(trade);
  const auto version = Snapshot().version;
  first.EmitTrade(trade);
  auto conflict = trade;
  conflict.price += 1;
  first.EmitTrade(conflict);
  conflict = trade;
  conflict.quantity = 1;
  first.EmitTrade(conflict);
  EXPECT_EQ(results.trades.size(), 1U);
  EXPECT_EQ(Snapshot().version, version);
  auto other_session = Trade("1", 1);
  other_session.trade_id.session = "day-2";
  first.EmitTrade(other_session);
  auto other_venue = Trade("1", 1);
  other_venue.trade_id.venue = "other";
  first.EmitTrade(other_venue);
  EXPECT_EQ(results.trades.size(), 3U);
  EXPECT_EQ(Snapshot().filled_quantity, 4);
}

TEST_F(TradeTest, CumulativeAndDetailTotalsAreMergedWithoutDoubleCounting) {
  first.Emit({{100, 1}, AdapterOrderState::kAccepted, 4});
  const auto version = Snapshot().version;
  first.EmitTrade(Trade("2", 2));
  first.EmitTrade(Trade("1", 1));
  EXPECT_EQ(Snapshot().filled_quantity, 4);
  EXPECT_EQ(Snapshot().version, version);
  first.Emit({{100, 1}, AdapterOrderState::kAccepted, 1});
  EXPECT_EQ(Snapshot().version, version);
  first.EmitTrade(Trade("3", 2));
  EXPECT_EQ(Snapshot().filled_quantity, 5);
  EXPECT_EQ(Snapshot().state, OrderState::kFilled);
  EXPECT_EQ(Snapshot().version, version + 1);
  first.Emit({{100, 1}, AdapterOrderState::kAccepted, 5});
  EXPECT_EQ(Snapshot().version, version + 1);
  EXPECT_EQ(results.trades.size(), 3U);
}

TEST_F(TradeTest, LateDetailsAfterFullCumulativeFillStillReachTheOwner) {
  first.Emit({{100, 1}, AdapterOrderState::kAccepted, 5});
  const auto version = Snapshot().version;
  first.EmitTrade(Trade("2", 3));
  first.EmitTrade(Trade("1", 2));
  first.EmitTrade(Trade("1", 2));
  EXPECT_EQ(results.trades.size(), 2U);
  EXPECT_EQ(Snapshot().version, version);
  EXPECT_EQ(Snapshot().filled_quantity, 5);
}

TEST_F(TradeTest, TradeBeforeAcceptanceSurvivesLateAcceptanceAndRejection) {
  first.EmitTrade(Trade("1"));
  EXPECT_EQ(Snapshot().state, OrderState::kPartiallyFilled);
  service.Run();
  first.Emit({{100, 1}, AdapterOrderState::kRejected, 0});
  EXPECT_EQ(Snapshot().state, OrderState::kPartiallyFilled);
  EXPECT_EQ(Snapshot().filled_quantity, 2);
  EXPECT_TRUE(results.orders.empty());
}

TEST_F(TradeTest, SameIdentityIsIndependentAcrossAccountsButNotOrders) {
  Submit({100, 2}, 8, foreign);
  Submit({100, 3}, 7, results);
  service.Run();
  first.EmitTrade(Trade("1"));
  auto trade = Trade("1", 3);
  trade.order_id = {100, 2};
  second.EmitTrade(trade);
  ASSERT_EQ(foreign.trades.size(), 1U);
  EXPECT_EQ(foreign.trades[0].snapshot.order.account_id, 8U);
  trade.order_id = {100, 3};
  first.EmitTrade(trade);
  EXPECT_EQ(Snapshot({100, 3}).filled_quantity, 0);
  EXPECT_EQ(results.trades.size(), 1U);
  EXPECT_EQ(Snapshot({100, 2}, 8).filled_quantity, 3);
}

TEST_F(TradeTest, InvalidTradesDoNotPoisonDeduplicationOrExistingState) {
  service.Run();
  std::vector<TradeInfo> invalid;
  auto trade = Trade("1");
  trade.quantity = 0;
  invalid.push_back(trade);
  trade.quantity = -1;
  invalid.push_back(trade);
  trade.quantity = 6;
  invalid.push_back(trade);
  trade = Trade("1");
  trade.price = std::numeric_limits<double>::infinity();
  invalid.push_back(trade);
  trade.price = std::numeric_limits<double>::quiet_NaN();
  invalid.push_back(trade);
  trade.price = 0;
  invalid.push_back(trade);
  trade = Trade("1");
  trade.trade_id.venue.clear();
  invalid.push_back(trade);
  trade = Trade("1");
  trade.trade_id.session.clear();
  invalid.push_back(trade);
  trade = Trade("1");
  trade.trade_id.value.clear();
  invalid.push_back(trade);
  trade = Trade("1");
  trade.order_id = {100, 99};
  invalid.push_back(trade);
  for (const auto& bad : invalid) {
    first.EmitTrade(bad);
  }
  second.EmitTrade(Trade("1"));
  EXPECT_EQ(Snapshot().filled_quantity, 0);
  EXPECT_TRUE(results.trades.empty());
  first.EmitTrade(Trade("1"));
  EXPECT_EQ(Snapshot().filled_quantity, 2);
  EXPECT_EQ(results.trades.size(), 1U);
}

TEST_F(TradeTest, OverfillAdditionCannotOverflowOrConsumeTheTradeId) {
  constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
  Submit({100, 2}, 7, results, kMax);
  service.Run();
  auto trade = Trade("1", kMax - 1);
  trade.order_id = {100, 2};
  first.EmitTrade(trade);
  trade.trade_id.value = "2";
  trade.quantity = kMax;
  first.EmitTrade(trade);
  EXPECT_EQ(Snapshot({100, 2}).filled_quantity, kMax - 1);
  trade.quantity = 1;
  first.EmitTrade(trade);
  EXPECT_EQ(Snapshot({100, 2}).filled_quantity, kMax);
  EXPECT_EQ(Snapshot({100, 2}).state, OrderState::kFilled);
  EXPECT_EQ(results.trades.size(), 2U);
}

TEST_F(TradeTest, CancelAndLateFillsRetainConfirmedFacts) {
  service.Run();
  first.EmitTrade(Trade("1"));
  EXPECT_EQ(client->CancelOrder({{100, 1}, 7, 2, 1}, results),
            RequestResult::kDelivered);
  service.Run();
  EXPECT_EQ(Snapshot().state, OrderState::kCanceled);
  first.EmitTrade(Trade("2", 1));
  EXPECT_EQ(Snapshot().state, OrderState::kCanceled);
  EXPECT_EQ(Snapshot().filled_quantity, 3);
  first.EmitTrade(Trade("3", 2));
  EXPECT_EQ(Snapshot().state, OrderState::kFilled);
}

TEST_F(TradeTest, RejectedOrderCannotAcquireTradeDetails) {
  first.Emit({{100, 1}, AdapterOrderState::kRejected, 0});
  first.EmitTrade(Trade("1"));
  EXPECT_TRUE(results.trades.empty());
  EXPECT_EQ(Snapshot().state, OrderState::kRejected);
  EXPECT_EQ(Snapshot().filled_quantity, 0);
}

TEST_F(TradeTest, CallbackRequestsAndNestedRunKeepBorrowedReportIntact) {
  service.Run();
  ASSERT_TRUE(first.Fill(Trade("1")));
  bool submitted = false;
  results.action = [&](const TradeReport& report) {
    const auto saved = report;
    service.Run();
    if (!submitted) {
      submitted = true;
      for (std::uint64_t id = 2; id <= 300; ++id) {
        Submit({100, id}, 7, foreign);
      }
      EXPECT_EQ(client->SubmitOrder(
          {{{100, 1}, 7, 2, 42, Side::kBuy, Offset::kOpen, 100, 5}},
          foreign), RequestResult::kDelivered);
    }
    EXPECT_EQ(report.trade, saved.trade);
    EXPECT_EQ(report.snapshot.version, saved.snapshot.version);
    EXPECT_EQ(report.snapshot.filled_quantity, saved.snapshot.filled_quantity);
  };
  service.Run();
  ASSERT_EQ(results.trades.size(), 1U);
  EXPECT_EQ(results.trades[0].trade, Trade("1"));
  EXPECT_TRUE(foreign.failures.empty());
  service.Run();
  ASSERT_EQ(foreign.failures.size(), 1U);
  EXPECT_EQ(foreign.failures[0].reason, FailureReason::kDuplicateOrder);
}

TEST_F(TradeTest,
       FullFillBeforeCancelResultCompletesTheAttemptWithoutRollback) {
  service.Run();
  EXPECT_EQ(client->CancelOrder({{100, 1}, 7, 2, 1}, results),
            RequestResult::kDelivered);
  ASSERT_TRUE(first.Fill(Trade("1", 5)));
  service.Run();
  ASSERT_EQ(results.cancels.size(), 1U);
  EXPECT_EQ(results.cancels[0].result, CancelResult::kRejected);
  EXPECT_EQ(Snapshot().state, OrderState::kFilled);
  EXPECT_EQ(Snapshot().filled_quantity, 5);
}

TEST_F(TradeTest, LateTradeCorrectsSendFailureAndPreservesQueuedSnapshots) {
  first.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  Submit({100, 2}, 7, results);
  auto trade = Trade("1", 2);
  trade.order_id = {100, 2};
  ::testing::internal::CaptureStderr();
  first.EmitTrade(trade);
  const auto diagnostic = ::testing::internal::GetCapturedStderr();
  EXPECT_NE(diagnostic.find("report_after_send_failure"), std::string::npos);
  trade.trade_id.value = "2";
  trade.quantity = 3;
  first.EmitTrade(trade);
  EXPECT_TRUE(results.trades.empty());
  service.Run();
  ASSERT_EQ(results.trades.size(), 2U);
  EXPECT_EQ(results.trades[0].snapshot.state, OrderState::kPartiallyFilled);
  EXPECT_EQ(results.trades[0].snapshot.filled_quantity, 2);
  EXPECT_EQ(results.trades[1].snapshot.state, OrderState::kFilled);
  EXPECT_EQ(results.trades[1].snapshot.filled_quantity, 5);
  ASSERT_EQ(results.failures.size(), 1U);
  ASSERT_TRUE(results.failures[0].snapshot);
  EXPECT_EQ(results.failures[0].snapshot->state, OrderState::kSendFailed);
  EXPECT_EQ(Snapshot({100, 2}).filled_quantity, 5);
}

TEST_F(TradeTest, TradeBatchIsBoundedAndDoesNotStarveAnotherAccount) {
  Submit({100, 2}, 7, results, 130);
  Submit({100, 3}, 8, foreign);
  service.Run();
  for (int id = 1; id <= 130; ++id) {
    auto trade = Trade("unused", 1);
    trade.order_id = {100, 2};
    trade.trade_id.value = std::to_string(id);
    ASSERT_TRUE(first.Fill(trade));
  }
  auto other = Trade("1");
  other.order_id = {100, 3};
  ASSERT_TRUE(second.Fill(other));
  service.Run();
  EXPECT_EQ(results.trades.size(), 64U);
  EXPECT_EQ(foreign.trades.size(), 1U);
  service.Run();
  EXPECT_EQ(results.trades.size(), 128U);
  service.Run();
  EXPECT_EQ(results.trades.size(), 130U);
  EXPECT_EQ(Snapshot({100, 2}).state, OrderState::kFilled);
  service.Run();
  EXPECT_EQ(results.trades.size(), 130U);
}

TEST_F(TradeTest, SimulatedBookRejectsInvalidAndRepeatedFillsWithoutCallbacks) {
  auto bad = Trade("1", 0);
  EXPECT_FALSE(first.Fill(bad));
  bad = Trade("1", 6);
  EXPECT_FALSE(first.Fill(bad));
  bad = Trade("1");
  bad.order_id = {100, 99};
  EXPECT_FALSE(first.Fill(bad));
  ASSERT_TRUE(first.Fill(Trade("1")));
  EXPECT_FALSE(first.Fill(Trade("1")));
  EXPECT_TRUE(results.trades.empty());
  service.Run();
  ASSERT_EQ(results.trades.size(), 1U);
  EXPECT_EQ(Snapshot().filled_quantity, 2);
  EXPECT_EQ(client->CancelOrder({{100, 1}, 7, 2, 1}, results),
            RequestResult::kDelivered);
  service.Run();
  EXPECT_FALSE(first.Fill(Trade("2", 1)));
}

TEST_F(TradeTest, CompleteIdentityIsNotTruncated) {
  service.Run();
  auto trade = Trade("unused", 1);
  trade.trade_id.value = std::string(300, 'x') + "1";
  first.EmitTrade(trade);
  trade.trade_id.value.back() = '2';
  first.EmitTrade(trade);
  EXPECT_EQ(Snapshot().filled_quantity, 2);
  ASSERT_EQ(results.trades.size(), 2U);
  EXPECT_NE(results.trades[0].trade.trade_id, results.trades[1].trade.trade_id);
}

}  // namespace
}  // namespace yquant::trading
