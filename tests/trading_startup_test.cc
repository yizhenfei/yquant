#include "yquant/trading/trading_service.h"

#include <array>
#include <optional>
#include <vector>

#include "trading_test_support.h"

#include "gtest/gtest.h"
#include "yquant/model/currency.h"
#include "yquant/trading/simulated_trading_adapter.h"

namespace yquant::trading {
namespace {

TEST(AccountStartup, StartsOnceAndKeepsThePublishedBaseline) {
  SimulatedTradingAdapter adapter;
  TradingService service;
  const FundsInfo funds{model::Currency("CNY"), 100, 100, 0, 0};
  adapter.SetInitialState(funds, {});
  ASSERT_TRUE(service.BindAccount(7, adapter));
  EXPECT_FALSE(service.IsAccountReady(7));
  service.Run();
  EXPECT_TRUE(service.IsAccountReady(7));
  EXPECT_EQ(adapter.sync_requests(), 1U);
  adapter.QueueFunds({2, FundsRefreshStarted{}});
  adapter.QueueFunds({2, FundsInfo{model::Currency("CNY"), 0, 0, 0, 0}});
  for (int i = 0; i < 3; ++i) {
    service.Run();
  }
  FundsSnapshot snapshot;
  ASSERT_TRUE(service.QueryFunds({7}, snapshot));
  EXPECT_EQ(snapshot.funds, funds);
  EXPECT_EQ(adapter.sync_requests(), 1U);
  EXPECT_FALSE(service.RetryAccountSync(7));
}

// Observes public request failures and account values during order callbacks.
class StartupListener final : public TradingListener {
 public:
  explicit StartupListener(TradingService& service) : service(service) {}
  // TradingListener implementation.
  void OnRequestFailure(const RequestFailure& failure) override {
    failures.push_back(failure.reason);
  }
  void OnOrderReport(const OrderReport&) override {
    EXPECT_TRUE(service.IsAccountReady(7));
    FundsSnapshot funds;
    EXPECT_TRUE(service.QueryFunds({7}, funds));
    ++orders;
  }
  void OnCancelReport(const CancelReport&) override {}
  void OnTradeReport(const TradeReport&) override { ++trades; }
  TradingService& service;
  std::vector<FailureReason> failures;
  int orders = 0;
  int trades = 0;
};

TEST(AccountStartup, NoOrderCanBeSentBeforeBoundaryConfirmation) {
  SimulatedTradingAdapter adapter;
  TradingService service;
  StartupListener listener(service);
  ASSERT_TRUE(service.BindAccount(7, adapter));
  test::ConfigureZeroCostTerms(service, 7);
  const SubmitOrderRequest request{{{100, 1}, 7, 2, 42, Side::kBuy,
                                    Offset::kOpen, 100, 1}};
  service.SubmitOrder(request, listener, {});
  EXPECT_TRUE(listener.failures.empty());
  adapter.QueueFunds({1, FundsRefreshStarted{}});
  adapter.QueueFunds({1, FundsInfo{model::Currency("CNY"), 100, 100, 0, 0}});
  adapter.QueuePositions({1, PositionsRefreshStarted{}});
  adapter.QueuePositions({1, PositionsRefreshCompleted{0}});
  service.Run();
  EXPECT_EQ(listener.failures, std::vector{FailureReason::kAccountNotReady});
  EXPECT_EQ(adapter.FindOrder(request.order.order_id), nullptr);
  EXPECT_FALSE(service.IsAccountReady(7));
  adapter.QueueAccountSync({1, AccountSyncResult::kComplete});
  service.Run();
  service.SubmitOrder(request, listener, {});
  service.Run();
  EXPECT_NE(adapter.FindOrder(request.order.order_id), nullptr);
  EXPECT_EQ(listener.orders, 1);
}

TEST(AccountStartup, FailedCompletionAndIncompleteSnapshotsRequireRetry) {
  SimulatedTradingAdapter adapter;
  TradingService service;
  ASSERT_TRUE(service.BindAccount(7, adapter));
  EXPECT_FALSE(service.RetryAccountSync(99));
  EXPECT_FALSE(service.RetryAccountSync(7));
  service.Run();
  EXPECT_FALSE(service.RetryAccountSync(7));
  adapter.QueueAccountSync({1, AccountSyncResult::kComplete});
  service.Run();
  EXPECT_FALSE(service.IsAccountReady(7));
  ASSERT_TRUE(service.RetryAccountSync(7));
  adapter.QueueAccountSync({2, AccountSyncResult::kFailed});
  service.Run();
  EXPECT_FALSE(service.IsAccountReady(7));
  adapter.SetInitialState({model::Currency("CNY"), 0, 0, 0, 0}, {});
  ASSERT_TRUE(service.RetryAccountSync(7));
  adapter.QueueAccountSync({1, AccountSyncResult::kComplete});
  service.Run();
  EXPECT_TRUE(service.IsAccountReady(7));
  EXPECT_EQ(adapter.sync_requests(), 3U);
}

TEST(AccountStartup, HistoricalFillIsAlreadyIncludedInInitialBaseline) {
  SimulatedTradingAdapter adapter;
  TradingService service;
  ASSERT_TRUE(service.BindAccount(7, adapter));
  const SubmitOrderRequest historical{{{99, 1}, 7, 2, 42, Side::kBuy,
                                       Offset::kOpen, 100, 1}};
  ASSERT_EQ(adapter.SubmitOrder(historical), SendResult::kSent);
  ASSERT_TRUE(adapter.Fill(
      {historical.order.order_id, {"exchange", "day", "trade1"}, 100, 1}));
  const std::array<PositionInfo, 1> positions{{{{42}, 1, 0, 0, 0}}};
  const FundsInfo funds{model::Currency("CNY"), 98, 88, 10, 0};
  adapter.SetInitialState(funds, positions);
  service.Run();
  ASSERT_TRUE(service.IsAccountReady(7));
  FundsSnapshot cash;
  PositionInfo position;
  ASSERT_TRUE(service.QueryFunds({7}, cash));
  ASSERT_TRUE(service.QueryPosition({7, {42}}, position));
  EXPECT_EQ(cash.funds, funds);
  EXPECT_EQ(position, positions[0]);
  // A late historical duplicate has no current order association to mutate.
  adapter.EmitTrade(
      {historical.order.order_id, {"exchange", "day", "trade1"}, 100, 1});
  ASSERT_TRUE(service.QueryFunds({7}, cash));
  ASSERT_TRUE(service.QueryPosition({7, {42}}, position));
  EXPECT_EQ(cash.funds, funds);
  EXPECT_EQ(position, positions[0]);
}

TEST(AccountStartup, ActivePreStartOrdersPreventReadinessUntilResolved) {
  SimulatedTradingAdapter adapter;
  TradingService service;
  ASSERT_TRUE(service.BindAccount(7, adapter));
  const SubmitOrderRequest historical{{{99, 1}, 7, 2, 42, Side::kBuy,
                                       Offset::kOpen, 100, 1}};
  ASSERT_EQ(adapter.SubmitOrder(historical), SendResult::kSent);
  adapter.SetInitialState({model::Currency("CNY"), 100, 100, 0, 0}, {});
  service.Run();
  EXPECT_FALSE(service.IsAccountReady(7));
  adapter.EmitCancel({historical.order.order_id, 1, CancelResult::kCanceled});
  ASSERT_TRUE(service.RetryAccountSync(7));
  service.Run();
  EXPECT_TRUE(service.IsAccountReady(7));
}

TEST(AccountStartup, UnassociatedFrozenAmountsCannotBeDeclaredReady) {
  for (bool funds_frozen : {false, true}) {
    SimulatedTradingAdapter adapter;
    TradingService service;
    ASSERT_TRUE(service.BindAccount(7, adapter));
    adapter.QueueFunds({1, FundsRefreshStarted{}});
    adapter.QueueFunds({1, FundsInfo{model::Currency("CNY"), 100, 90, 0,
                                   funds_frozen ? 10.0 : 0.0}});
    adapter.QueuePositions({1, PositionsRefreshStarted{}});
    adapter.QueuePositions({1, PositionInfo{{42}, 1, 0,
                                           funds_frozen ? 0 : 1, 0}});
    adapter.QueuePositions({1, PositionsRefreshCompleted{1}});
    adapter.QueueAccountSync({1, AccountSyncResult::kComplete});
    service.Run();
    EXPECT_FALSE(service.IsAccountReady(7));
  }
}

// Rejects the first request to verify that Run never busy-retries a failure.
class RejectingStartupAdapter final : public ITradingAdapter {
 public:
  // ITradingAdapter implementation.
  void SetCallback(ITradingAdapterCallback&) override {}
  bool StartAccountSync(std::uint64_t id) override {
    ids.push_back(id);
    return false;
  }
  SendResult SubmitOrder(const SubmitOrderRequest&) override {
    ADD_FAILURE();
    return SendResult::kNotSent;
  }
  SendResult CancelOrder(const AdapterCancelOrderRequest&) override {
    return SendResult::kNotSent;
  }
  void Run() override {}
  std::vector<std::uint64_t> ids;
};

TEST(AccountStartup, RejectedSyncRequestCanBeRetriedWithANewId) {
  RejectingStartupAdapter adapter;
  TradingService service;
  ASSERT_TRUE(service.BindAccount(7, adapter));
  service.Run();
  service.Run();
  EXPECT_EQ(adapter.ids, (std::vector<std::uint64_t>{1}));
  ASSERT_TRUE(service.RetryAccountSync(7));
  service.Run();
  EXPECT_EQ(adapter.ids, (std::vector<std::uint64_t>{1, 2}));
  EXPECT_FALSE(service.IsAccountReady(7));
}

}  // namespace
}  // namespace yquant::trading
