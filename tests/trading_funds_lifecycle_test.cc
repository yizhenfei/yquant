#include "yquant/trading/trading_service.h"

#include <array>
#include <limits>
#include <functional>
#include <optional>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/model/currency.h"
#include "yquant/trading/funds_terms.h"
#include "yquant/trading/order.h"
#include "yquant/trading/simulated_trading_adapter.h"

namespace yquant::trading {
namespace {

// Collects public trading events while the fixture inspects account balances.
class FundsEvents final : public TradingListener {
 public:
  // TradingListener implementation.
  void OnRequestFailure(const RequestFailure& value) override {
    failures.push_back(value.reason);
  }
  void OnOrderReport(const OrderReport&) override {}
  void OnCancelReport(const CancelReport& report) override {
    if (on_cancel) on_cancel(report);
  }
  void OnTradeReport(const TradeReport&) override {}
  std::vector<FailureReason> failures;
  std::function<void(const CancelReport&)> on_cancel;
};

// Runs real adapter requests against a funded account with nonzero order costs.
class FundsLifecycleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(service.BindAccount(7, adapter));
    FundsTerms terms;
    terms.currency = model::Currency("CNY");
    terms.contract_multiplier = 1;
    terms.margin_rate = 0.1;
    for (auto& fee : terms.fees) {
      fee = FeeRate{1, 0};
    }
    ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kBuy, terms));
    ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kSell, terms));
    adapter.SetInitialState({terms.currency, 1000, 1000, 0, 0}, {});
    service.Run();
  }
  FundsSnapshot Funds() {
    FundsSnapshot funds;
    EXPECT_TRUE(service.QueryFunds({7}, funds));
    return funds;
  }
  void Submit() { service.SubmitOrder(request, events, {}); }
  SimulatedTradingAdapter adapter;
  FundsEvents events;
  TradingService service;
  SubmitOrderRequest request{{{100, 1}, 7, 2, 42, Side::kBuy,
                              Offset::kOpen, 100, 10}};
};

TEST_F(FundsLifecycleTest, CounterRejectionReleasesReservationOnce) {
  adapter.SetNextSubmit({SendResult::kSent, AdapterOrderState::kRejected});
  Submit();
  EXPECT_DOUBLE_EQ(Funds().reserved, 110);
  service.Run();
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
  EXPECT_DOUBLE_EQ(Funds().available_for_orders(), 1000);
  adapter.Emit({request.order.order_id, AdapterOrderState::kRejected});
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
  EXPECT_DOUBLE_EQ(Funds().available_for_orders(), 1000);
}

TEST_F(FundsLifecycleTest, CancelKeepsFundsUntilSuccessfulConfirmation) {
  Submit();
  service.Run();
  adapter.SetNextCancel({SendResult::kSent, CancelResult::kRejected});
  service.CancelOrder({request.order.order_id, 7, 2, 1}, events, {});
  EXPECT_DOUBLE_EQ(Funds().reserved, 110);
  service.Run();
  EXPECT_DOUBLE_EQ(Funds().reserved, 110);
  service.CancelOrder({request.order.order_id, 7, 2, 2}, events, {});
  EXPECT_DOUBLE_EQ(Funds().reserved, 110);
  service.Run();
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
  adapter.EmitCancel({request.order.order_id, 2, CancelResult::kCanceled});
  EXPECT_DOUBLE_EQ(Funds().available_for_orders(), 1000);
}

TEST_F(FundsLifecycleTest, PartialCancelRetainsFilledEstimateAndLateFills) {
  Submit();
  service.Run();
  adapter.Emit({request.order.order_id, AdapterOrderState::kAccepted, 4});
  service.CancelOrder({request.order.order_id, 7, 2, 1}, events, {});
  service.Run();
  FundsSnapshot unavailable;
  EXPECT_FALSE(service.QueryFunds({7}, unavailable));
  adapter.Emit({request.order.order_id, AdapterOrderState::kAccepted, 4});
  EXPECT_FALSE(service.QueryFunds({7}, unavailable));
  adapter.Emit({request.order.order_id, AdapterOrderState::kAccepted, 5});
  EXPECT_FALSE(service.QueryFunds({7}, unavailable));
  adapter.EmitTrade({request.order.order_id, {"sim", "day", "priced"}, 100, 5});
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
  EXPECT_DOUBLE_EQ(Funds().funds.margin, 50);
  EXPECT_DOUBLE_EQ(Funds().commission, 5);
}

TEST_F(FundsLifecycleTest, LateAcceptanceRestoresDefinitelyNotSentReservation) {
  adapter.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  Submit();
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
  adapter.Emit({request.order.order_id, AdapterOrderState::kAccepted});
  EXPECT_DOUBLE_EQ(Funds().reserved, 110);
  adapter.Emit({request.order.order_id, AdapterOrderState::kAccepted});
  EXPECT_DOUBLE_EQ(Funds().reserved, 110);
}

TEST_F(FundsLifecycleTest, OpeningFillBooksActualMarginAndFeeOnce) {
  Submit();
  service.Run();
  const TradeInfo trade{request.order.order_id, {"sim", "day", "one"}, 90, 2};
  adapter.EmitTrade(trade);
  auto funds = Funds();
  EXPECT_DOUBLE_EQ(funds.reserved, 88);
  EXPECT_DOUBLE_EQ(funds.funds.margin, 18);
  EXPECT_DOUBLE_EQ(funds.funds.balance, 998);
  EXPECT_DOUBLE_EQ(funds.funds.available, 980);
  adapter.EmitTrade(trade);
  EXPECT_EQ(Funds(), funds);
}

TEST_F(FundsLifecycleTest, CloseTodayReleasesWeightedCostMarginAndBooksProfit) {
  request.order.quantity = 2;
  Submit();
  service.Run();
  adapter.EmitTrade({request.order.order_id, {"sim", "day", "one"}, 90, 1});
  adapter.EmitTrade({request.order.order_id, {"sim", "day", "two"}, 110, 1});
  request.order =
      {{100, 2}, 7, 2, 42, Side::kSell, Offset::kCloseToday, 120, 1};
  Submit();
  service.Run();
  adapter.EmitTrade({request.order.order_id, {"sim", "day", "three"}, 120, 1});
  const auto funds = Funds();
  EXPECT_DOUBLE_EQ(funds.reserved, 0);
  EXPECT_DOUBLE_EQ(funds.funds.balance, 1017);
  EXPECT_DOUBLE_EQ(funds.funds.margin, 10);
  EXPECT_DOUBLE_EQ(funds.funds.available, 1007);
}

TEST_F(FundsLifecycleTest, CancelAfterPartialPricedFillOnlyReleasesRemainder) {
  Submit();
  service.Run();
  adapter.EmitTrade({request.order.order_id, {"sim", "day", "one"}, 90, 4});
  EXPECT_DOUBLE_EQ(Funds().reserved, 66);
  service.CancelOrder({request.order.order_id, 7, 2, 1}, events, {});
  service.Run();
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
  EXPECT_DOUBLE_EQ(Funds().funds.margin, 36);
  EXPECT_DOUBLE_EQ(Funds().available_for_orders(), 960);
  adapter.EmitTrade({request.order.order_id, {"sim", "day", "late"}, 100, 1});
  EXPECT_DOUBLE_EQ(Funds().funds.margin, 46);
  EXPECT_DOUBLE_EQ(Funds().commission, 5);
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
}

TEST_F(FundsLifecycleTest, CumulativeAndDetailOrderDoNotDuplicateCashFlows) {
  Submit();
  service.Run();
  adapter.Emit({request.order.order_id, AdapterOrderState::kAccepted, 2});
  FundsSnapshot sentinel{{model::Currency("USD"), 123, 123, 0, 0}};
  auto output = sentinel;
  EXPECT_FALSE(service.QueryFunds({7}, output));
  EXPECT_EQ(output, sentinel);
  auto another = request;
  another.order.order_id.sequence = 2;
  service.SubmitOrder(another, events, {});
  service.Run();
  ASSERT_EQ(events.failures.size(), 1U);
  EXPECT_EQ(events.failures[0], FailureReason::kFundsUnavailable);
  adapter.EmitTrade({request.order.order_id, {"sim", "day", "one"}, 90, 2});
  const auto booked = Funds();
  EXPECT_DOUBLE_EQ(booked.reserved, 88);
  adapter.Emit({request.order.order_id, AdapterOrderState::kAccepted, 2});
  adapter.Emit({request.order.order_id, AdapterOrderState::kAccepted, 1});
  EXPECT_EQ(Funds(), booked);
  adapter.EmitTrade({request.order.order_id, {"sim", "day", "two"}, 100, 2});
  const auto next = Funds();
  adapter.Emit({request.order.order_id, AdapterOrderState::kAccepted, 4});
  EXPECT_EQ(Funds(), next);
  EXPECT_DOUBLE_EQ(next.commission, 4);
}

TEST_F(FundsLifecycleTest,
       LateFillAfterSendFailureBooksCashAndRestoresRemainder) {
  adapter.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  Submit();
  const TradeInfo trade{request.order.order_id, {"sim", "day", "late"}, 100, 4};
  adapter.EmitTrade(trade);
  EXPECT_DOUBLE_EQ(Funds().reserved, 66);
  EXPECT_DOUBLE_EQ(Funds().funds.margin, 40);
  EXPECT_DOUBLE_EQ(Funds().funds.balance, 996);
  const auto booked = Funds();
  adapter.EmitTrade(trade);
  EXPECT_EQ(Funds(), booked);
}

TEST_F(FundsLifecycleTest, FullFillDuringCancelDoesNotReleaseOccupiedMargin) {
  Submit();
  service.Run();
  service.CancelOrder({request.order.order_id, 7, 2, 1}, events, {});
  adapter.EmitTrade({request.order.order_id, {"sim", "day", "all"}, 100, 10});
  const auto filled = Funds();
  service.Run();
  EXPECT_EQ(Funds(), filled);
  EXPECT_DOUBLE_EQ(filled.reserved, 0);
  EXPECT_DOUBLE_EQ(filled.funds.margin, 100);
  EXPECT_DOUBLE_EQ(filled.funds.available, 890);
}

TEST_F(FundsLifecycleTest, MissingReportsAndFailedCancelKeepTheHold) {
  adapter.SetNextSubmit({SendResult::kSent, std::nullopt});
  Submit();
  const auto held = Funds();
  for (int i = 0; i < 5; ++i) service.Run();
  adapter.SetNextCancel({SendResult::kNotSent, std::nullopt});
  service.CancelOrder({request.order.order_id, 7, 2, 1}, events, {});
  service.Run();
  EXPECT_EQ(Funds(), held);
}

TEST_F(FundsLifecycleTest, ReusedTradeIdWithDifferentDataCannotChangeLedger) {
  Submit();
  service.Run();
  TradeInfo trade{request.order.order_id, {"sim", "day", "one"}, 100, 1};
  adapter.EmitTrade(trade);
  const auto booked = Funds();
  trade.price = 110;
  adapter.EmitTrade(trade);
  EXPECT_EQ(Funds(), booked);
}

TEST_F(FundsLifecycleTest,
       InvalidAccountingPreservesTradeFactAndBlocksNewFundsUse) {
  request.order.offset = Offset::kCloseToday;
  Submit();
  service.Run();
  adapter.EmitTrade({request.order.order_id, {"sim", "day", "close"}, 100, 10});
  FundsSnapshot cash;
  EXPECT_FALSE(service.QueryFunds({7}, cash));
  OrderSnapshot order;
  ASSERT_TRUE(service.QueryOrder({request.order.order_id, 7, 2}, order));
  EXPECT_EQ(order.state, OrderState::kFilled);
  request.order.order_id.sequence = 2;
  request.order.offset = Offset::kOpen;
  Submit();
  service.Run();
  EXPECT_EQ(events.failures.back(), FailureReason::kFundsUnavailable);
}

// Initializes both cost buckets and uses the same public trading entry points.
class InitialCostsTest : public ::testing::Test {
 protected:
  void Ready(PositionSide side = PositionSide::kLong,
             bool today_first = false) {
    ASSERT_TRUE(service.BindAccount(7, adapter));
    FundsTerms terms;
    terms.currency = model::Currency("CNY");
    terms.contract_multiplier = 1;
    terms.margin_rate = 0.1;
    terms.close_today_first = today_first;
    for (auto& fee : terms.fees) fee = FeeRate{1, 0};
    ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kBuy, terms));
    ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kSell, terms));
    position = {{42, side, HedgeFlag::kHedge}, 2, 3, 0, 0,
                PositionCost{200, 20}, PositionCost{240, 24}};
    const std::array<PositionInfo, 1> positions{position};
    adapter.SetInitialState({terms.currency, 1000, 956, 44, 0}, positions);
    service.Run();
    ASSERT_TRUE(service.IsAccountReady(7));
  }
  void Close(Offset offset, std::int32_t quantity, Side side = Side::kSell) {
    const SubmitOrderRequest request{{{100, 1}, 7, 2, 42, side,
        offset, 90, quantity, HedgeFlag::kHedge}};
    service.SubmitOrder(request, events, {});
    service.Run();
    adapter.EmitTrade({request.order.order_id, {"sim", "day", "close"},
                       90, quantity});
  }
  FundsSnapshot Funds() {
    FundsSnapshot funds;
    EXPECT_TRUE(service.QueryFunds({7}, funds));
    return funds;
  }
  SimulatedTradingAdapter adapter;
  FundsEvents events;
  TradingService service;
  PositionInfo position;
};

TEST_F(InitialCostsTest, CloseYesterdayUsesItsOwnBasisAndRetainsToday) {
  Ready();
  Close(Offset::kCloseYesterday, 2);
  EXPECT_DOUBLE_EQ(Funds().funds.balance, 1018);
  EXPECT_DOUBLE_EQ(Funds().funds.margin, 28);
  EXPECT_DOUBLE_EQ(Funds().funds.available, 990);
  PositionInfo after;
  ASSERT_TRUE(service.QueryPosition({7, position.key}, after));
  EXPECT_EQ(after.today_quantity, 2);
  EXPECT_EQ(after.today_cost, position.today_cost);
  EXPECT_EQ(after.yesterday_quantity, 1);
  ASSERT_TRUE(after.yesterday_cost);
  EXPECT_DOUBLE_EQ(after.yesterday_cost->cost, 80);
  EXPECT_DOUBLE_EQ(after.yesterday_cost->margin, 8);
}

TEST_F(InitialCostsTest, GenericCloseDefaultsToYesterdayFirst) {
  Ready();
  Close(Offset::kClose, 4);
  EXPECT_DOUBLE_EQ(Funds().funds.balance, 1016);
  EXPECT_DOUBLE_EQ(Funds().funds.margin, 10);
  EXPECT_DOUBLE_EQ(Funds().funds.available, 1006);
}

TEST_F(InitialCostsTest, GenericCloseCanUseTodayFirst) {
  Ready(PositionSide::kLong, true);
  Close(Offset::kClose, 4);
  EXPECT_DOUBLE_EQ(Funds().funds.balance, 996);
  EXPECT_DOUBLE_EQ(Funds().funds.margin, 8);
  EXPECT_DOUBLE_EQ(Funds().funds.available, 988);
}

TEST_F(InitialCostsTest, ClosingShortReversesTheProfitSign) {
  Ready(PositionSide::kShort);
  Close(Offset::kCloseToday, 2, Side::kBuy);
  EXPECT_DOUBLE_EQ(Funds().realized_pnl, 20);
  EXPECT_DOUBLE_EQ(Funds().funds.balance, 1018);
  EXPECT_DOUBLE_EQ(Funds().funds.margin, 24);
}

TEST_F(InitialCostsTest, FinalCloseRemovesAllCostAndMarginResidue) {
  Ready();
  Close(Offset::kClose, 5);
  EXPECT_DOUBLE_EQ(Funds().funds.margin, 0);
  EXPECT_DOUBLE_EQ(Funds().funds.balance, 1005);
  EXPECT_DOUBLE_EQ(Funds().funds.available, 1005);
  PositionInfo after;
  ASSERT_TRUE(service.QueryPosition({7, position.key}, after));
  EXPECT_EQ(after.today_quantity, 0);
  EXPECT_EQ(after.yesterday_quantity, 0);
  EXPECT_FALSE(after.today_cost);
  EXPECT_FALSE(after.yesterday_cost);
}

TEST(InitialCosts, NonemptyBucketRequiresFiniteCostAndNonnegativeMargin) {
  for (auto cost : {std::optional<PositionCost>{},
       std::optional{PositionCost{0, 1}},
       std::optional{PositionCost{-1, 1}},
       std::optional{PositionCost{100, -1}},
       std::optional{PositionCost{
           std::numeric_limits<double>::infinity(), 1}}}) {
    SimulatedTradingAdapter adapter;
    TradingService service;
    ASSERT_TRUE(service.BindAccount(7, adapter));
    const std::array<PositionInfo, 1> positions{{{{42}, 1, 0, 0, 0, cost}}};
    adapter.SetInitialState(
        {model::Currency("CNY"), 1000, 999, 1, 0}, positions);
    service.Run();
    EXPECT_FALSE(service.IsAccountReady(7));
  }
}

TEST(FundsRounding, ClosingAllBucketsDoesNotLeaveNegativeRoundingMargin) {
  SimulatedTradingAdapter adapter;
  TradingService service;
  FundsEvents events;
  ASSERT_TRUE(service.BindAccount(7, adapter));
  FundsTerms terms;
  terms.currency = model::Currency("CNY");
  terms.contract_multiplier = 1;
  terms.margin_rate = 0;
  for (auto& fee : terms.fees) fee = FeeRate{};
  ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kSell, terms));
  const std::array<PositionInfo, 1> positions{{
      {{42}, 2, 3, 0, 0, PositionCost{200, 0.1}, PositionCost{300, 0.2}}}};
  adapter.SetInitialState({terms.currency, 1000, 999.7, 0.3, 0}, positions);
  service.Run();
  SubmitOrderRequest request{{{100, 1}, 7, 2, 42, Side::kSell,
                              Offset::kCloseToday, 100, 2}};
  service.SubmitOrder(request, events, {});
  service.Run();
  adapter.EmitTrade({request.order.order_id, {"sim", "day", "today"}, 100, 2});
  request.order.order_id.sequence = 2;
  request.order.offset = Offset::kCloseYesterday;
  request.order.quantity = 3;
  service.SubmitOrder(request, events, {});
  service.Run();
  adapter.EmitTrade(
      {request.order.order_id, {"sim", "day", "yesterday"}, 100, 3});
  FundsSnapshot funds;
  ASSERT_TRUE(service.QueryFunds({7}, funds));
  EXPECT_DOUBLE_EQ(funds.funds.margin, 0);
  EXPECT_NEAR(funds.funds.available, 1000, 1e-10);
}

TEST_F(FundsLifecycleTest, ActualPriceUsesMultiplierAndBothFeeComponents) {
  FundsTerms terms;
  terms.currency = model::Currency("CNY");
  terms.contract_multiplier = 10;
  terms.margin_rate = 0.1;
  terms.fees[0] = FeeRate{2, 0.001};
  ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kBuy, terms));
  request.order.quantity = 2;
  Submit();
  service.Run();
  adapter.EmitTrade({request.order.order_id, {"sim", "day", "one"}, 90, 1});
  EXPECT_DOUBLE_EQ(Funds().reserved, 103);
  EXPECT_DOUBLE_EQ(Funds().funds.margin, 90);
  EXPECT_DOUBLE_EQ(Funds().commission, 2.9);
  EXPECT_DOUBLE_EQ(Funds().funds.available, 907.1);
}

TEST_F(FundsLifecycleTest, AccountsAndHedgeCategoriesDoNotShareAccounting) {
  SimulatedTradingAdapter second;
  ASSERT_TRUE(service.BindAccount(8, second));
  FundsTerms terms;
  terms.currency = model::Currency("CNY");
  terms.contract_multiplier = 1;
  terms.margin_rate = 0.1;
  terms.fees[0] = FeeRate{1, 0};
  ASSERT_TRUE(service.ConfigureFundsTerms(8, 42, Side::kBuy, terms));
  second.SetInitialState({terms.currency, 2000, 2000, 0, 0}, {});
  service.Run();
  request.order.hedge = HedgeFlag::kArbitrage;
  Submit();
  service.Run();
  adapter.EmitTrade({request.order.order_id, {"sim", "day", "one"}, 100, 10});
  PositionInfo position;
  ASSERT_TRUE(service.QueryPosition({7, {42}}, position));
  EXPECT_EQ(position.today_quantity, 0);
  ASSERT_TRUE(service.QueryPosition(
      {7, {42, PositionSide::kLong, HedgeFlag::kArbitrage}}, position));
  EXPECT_EQ(position.today_quantity, 10);
  FundsSnapshot other;
  ASSERT_TRUE(service.QueryFunds({8}, other));
  EXPECT_DOUBLE_EQ(other.funds.balance, 2000);
  EXPECT_DOUBLE_EQ(other.funds.margin, 0);
  request.order.account_id = 8;
  request.order.order_id.sequence = 2;
  Submit();
  service.Run();
  second.EmitTrade({request.order.order_id, {"sim", "day", "one"}, 100, 10});
  ASSERT_TRUE(service.QueryFunds({8}, other));
  EXPECT_DOUBLE_EQ(other.commission, 10);
  EXPECT_DOUBLE_EQ(Funds().commission, 10);
}

TEST_F(FundsLifecycleTest, CancelFinalQuantityRetainsUnpricedFilledFunds) {
  Submit();
  service.Run();
  adapter.SetNextCancel({SendResult::kSent, std::nullopt});
  service.CancelOrder({request.order.order_id, 7, 2, 1}, events, {});
  adapter.EmitCancel({request.order.order_id, 1, CancelResult::kCanceled, 4});
  FundsSnapshot funds;
  EXPECT_FALSE(service.QueryFunds({7}, funds));
  OrderSnapshot order;
  ASSERT_TRUE(service.QueryOrder({request.order.order_id, 7, 2}, order));
  EXPECT_EQ(order.filled_quantity, 4);
  EXPECT_EQ(order.state, OrderState::kCanceled);
  adapter.EmitTrade({request.order.order_id, {"sim", "day", "late"}, 100, 4});
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
  EXPECT_DOUBLE_EQ(Funds().funds.margin, 40);
  EXPECT_DOUBLE_EQ(Funds().funds.available, 956);
}

TEST_F(FundsLifecycleTest, InvalidFinalQuantityCannotReleaseCancelHold) {
  Submit();
  service.Run();
  adapter.SetNextCancel({SendResult::kSent, std::nullopt});
  service.CancelOrder({request.order.order_id, 7, 2, 1}, events, {});
  adapter.EmitCancel({request.order.order_id, 1, CancelResult::kCanceled, 11});
  EXPECT_DOUBLE_EQ(Funds().reserved, 110);
  adapter.EmitCancel({request.order.order_id, 1, CancelResult::kCanceled, 0});
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
}

TEST_F(FundsLifecycleTest, SimulatedCancelIncludesFillsQueuedBehindItsResult) {
  Submit();
  service.Run();
  service.CancelOrder({request.order.order_id, 7, 2, 1}, events, {});
  ASSERT_TRUE(adapter.Fill(
      {request.order.order_id, {"sim", "day", "pending"}, 100, 4}));
  bool canceled = false;
  events.on_cancel = [&](const CancelReport& report) {
    canceled = true;
    EXPECT_EQ(report.snapshot.filled_quantity, 4);
    FundsSnapshot funds;
    EXPECT_FALSE(service.QueryFunds({7}, funds));
  };
  service.Run();
  EXPECT_TRUE(canceled);
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
  EXPECT_DOUBLE_EQ(Funds().funds.margin, 40);
}

TEST_F(FundsLifecycleTest,
       ConflictingCancelFinalQuantityMakesFundsUnavailable) {
  Submit();
  service.Run();
  service.CancelOrder({request.order.order_id, 7, 2, 1}, events, {});
  service.Run();
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
  adapter.EmitCancel({request.order.order_id, 1, CancelResult::kCanceled, 4});
  FundsSnapshot funds;
  EXPECT_FALSE(service.QueryFunds({7}, funds));
}

}  // namespace
}  // namespace yquant::trading
