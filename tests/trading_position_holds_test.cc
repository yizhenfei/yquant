#include "yquant/trading/trading_client_factory.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/model/currency.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {
namespace {

// Records refusal reasons through the same callbacks used by strategies.
class Results final : public TradingListener {
 public:
  // TradingListener implementation.
  void OnRequestFailure(const RequestFailure& f) override {
    failures.push_back(f.reason);
  }
  void OnOrderReport(const OrderReport&) override {}
  void OnCancelReport(const CancelReport&) override {}
  void OnTradeReport(const TradeReport&) override {}
  std::vector<FailureReason> failures;
};

// Two strategies share ten long contracts: four today and six yesterday.
class PositionHoldsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(service.BindAccount(7, adapter));
    ASSERT_TRUE(registry.Register(3, service));
    TradingClientFactory factory(registry);
    a = factory.Create(3, results);
    b = factory.Create(3, results);
    FundsTerms terms;
    terms.currency = model::Currency("CNY");
    terms.contract_multiplier = 1;
    terms.margin_rate = 0.1;
    for (auto& fee : terms.fees) fee = FeeRate{1, 0};
    for (auto side : {Side::kSell, Side::kBuy}) {
      ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, side, terms));
    }
    const std::array<PositionInfo, 1> rows{{
        {{42}, 4, 6, 0, 0, PositionCost{400, 40}, PositionCost{480, 48}}}};
    adapter.SetInitialState({terms.currency, 1000, 912, 88, 0}, rows);
    service.Run();
  }
  void Submit(TradingClient& client, std::uint64_t seq, int quantity,
              Offset offset = Offset::kClose, Side side = Side::kSell,
              HedgeFlag hedge = HedgeFlag::kSpeculation) {
    client.SubmitOrder({{{1, seq}, 7, &client == a.get() ? 2U : 3U, 42,
                         side, offset, 100, quantity, hedge}});
  }
  PositionInfo Position() {
    PositionInfo row;
    EXPECT_TRUE(b->QueryPosition({7, {42}}, row));
    return row;
  }
  SimulatedTradingAdapter adapter, second_account;
  Results results;
  TradingService service;
  TradingServiceRegistry registry;
  std::unique_ptr<TradingClient> a, b;
};

TEST_F(PositionHoldsTest, TwoStrategiesCannotCloseTheSameSevenContracts) {
  Submit(*a, 1, 7);
  EXPECT_EQ(Position().yesterday_frozen, 6);
  EXPECT_EQ(Position().today_frozen, 1);
  Submit(*b, 2, 7);
  service.Run();
  EXPECT_EQ(adapter.FindOrder({1, 2}), nullptr);
  ASSERT_EQ(results.failures.size(), 1U);
  EXPECT_EQ(results.failures[0], FailureReason::kInsufficientPosition);
  OrderSnapshot missing;
  EXPECT_FALSE(b->QueryOrder({{1, 2}, 7, 3}, missing));
  FundsSnapshot funds;
  ASSERT_TRUE(a->QueryFunds({7}, funds));
  EXPECT_DOUBLE_EQ(funds.reserved, 7);
  EXPECT_EQ(Position().available_quantity(), 3);
}

TEST_F(PositionHoldsTest, PartialFillCancelAndReuse) {
  Submit(*a, 1, 7);
  service.Run();
  ASSERT_TRUE(adapter.Fill({{1, 1}, {"sim", "day", "a"}, 100, 3}));
  service.Run();
  EXPECT_EQ(Position().yesterday_quantity, 3);
  EXPECT_EQ(Position().yesterday_frozen, 3);
  EXPECT_EQ(Position().today_frozen, 1);
  a->CancelOrder({{1, 1}, 7, 2, 1});
  EXPECT_EQ(Position().available_quantity(), 3);
  service.Run();
  EXPECT_EQ(Position().available_quantity(), 7);
  Submit(*b, 2, 7);
  EXPECT_EQ(Position().available_quantity(), 0);
}

TEST_F(PositionHoldsTest, FillsConsumeTheirOwnBucketsInReverseOrder) {
  Submit(*a, 1, 6);
  Submit(*b, 2, 4);
  service.Run();
  const TradeInfo fill{{1, 2}, {"sim", "day", "b"}, 100, 4};
  adapter.EmitTrade(fill);
  adapter.EmitTrade(fill);
  adapter.Emit({{1, 2}, AdapterOrderState::kAccepted, 4});
  EXPECT_EQ(Position().today_quantity, 0);
  EXPECT_EQ(Position().yesterday_quantity, 6);
  EXPECT_EQ(Position().yesterday_frozen, 6);
  FundsSnapshot funds;
  ASSERT_TRUE(b->QueryFunds({7}, funds));
  EXPECT_DOUBLE_EQ(funds.realized_pnl, 0);
}

TEST_F(PositionHoldsTest, FixedBucketsAndWrongKeysCannotConsumeOtherHoldings) {
  Submit(*a, 1, 5, Offset::kCloseToday);
  Submit(*a, 2, 7, Offset::kCloseYesterday);
  Submit(*b, 3, 1, Offset::kClose, Side::kBuy);
  Submit(*b, 4, 1, Offset::kClose, Side::kSell, HedgeFlag::kHedge);
  service.Run();
  ASSERT_EQ(results.failures.size(), 4U);
  for (auto reason : results.failures) {
    EXPECT_EQ(reason, FailureReason::kInsufficientPosition);
  }
  EXPECT_EQ(Position().available_quantity(), 10);
}

TEST_F(PositionHoldsTest, FailedSendAndRejectionReleaseBothReservations) {
  adapter.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  Submit(*a, 1, 7);
  EXPECT_EQ(Position().available_quantity(), 10);
  adapter.SetNextSubmit({SendResult::kSent, AdapterOrderState::kRejected});
  Submit(*b, 2, 7);
  EXPECT_EQ(Position().available_quantity(), 3);
  service.Run();
  adapter.Emit({{1, 2}, AdapterOrderState::kRejected});
  EXPECT_EQ(Position().available_quantity(), 10);
  FundsSnapshot funds;
  ASSERT_TRUE(b->QueryFunds({7}, funds));
  EXPECT_DOUBLE_EQ(funds.reserved, 0);
}

TEST_F(PositionHoldsTest, FailedCancelsAndSilenceKeepTheOrderFrozen) {
  Submit(*a, 1, 7);
  service.Run();
  adapter.SetNextCancel({SendResult::kNotSent, std::nullopt});
  a->CancelOrder({{1, 1}, 7, 2, 1});
  service.Run();
  adapter.SetNextCancel({SendResult::kSent, CancelResult::kRejected});
  a->CancelOrder({{1, 1}, 7, 2, 2});
  service.Run();
  for (int i = 0; i < 10; ++i) service.Run();
  EXPECT_EQ(Position().available_quantity(), 3);
}

TEST_F(PositionHoldsTest, CumulativeFillWaitsForPriceEvenAfterCancellation) {
  Submit(*a, 1, 7);
  service.Run();
  adapter.Emit({{1, 1}, AdapterOrderState::kAccepted, 3});
  PositionInfo untouched{{99}};
  EXPECT_FALSE(b->QueryPosition({7, {42}}, untouched));
  EXPECT_EQ(untouched.key.instrument_id, 99U);
  Submit(*b, 2, 1);
  EXPECT_EQ(adapter.FindOrder({1, 2}), nullptr);
  a->CancelOrder({{1, 1}, 7, 2, 1});
  service.Run();
  adapter.EmitTrade({{1, 1}, {"sim", "day", "late"}, 100, 3});
  EXPECT_EQ(Position().available_quantity(), 7);
  EXPECT_EQ(Position().yesterday_frozen, 0);
}

TEST_F(PositionHoldsTest, LateAcceptanceRestoresTheOriginalBucketsOnlyOnce) {
  adapter.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  Submit(*a, 1, 7);
  adapter.Emit({{1, 1}, AdapterOrderState::kAccepted});
  adapter.Emit({{1, 1}, AdapterOrderState::kAccepted});
  EXPECT_EQ(Position().available_quantity(), 3);
  adapter.EmitTrade({{1, 1}, {"sim", "day", "late"}, 100, 7});
  EXPECT_EQ(Position().available_quantity(), 3);
  EXPECT_EQ(Position().today_quantity, 3);
  EXPECT_EQ(Position().yesterday_quantity, 0);
}

TEST_F(PositionHoldsTest, LateFillCannotStealAnotherOrdersReservation) {
  adapter.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  Submit(*a, 1, 7);
  Submit(*b, 2, 6);
  adapter.EmitTrade({{1, 1}, {"sim", "day", "late"}, 100, 7});
  PositionInfo unavailable;
  EXPECT_FALSE(a->QueryPosition({7, {42}}, unavailable));
  OrderSnapshot fact;
  ASSERT_TRUE(a->QueryOrder({{1, 1}, 7, 2}, fact));
  EXPECT_EQ(fact.state, OrderState::kFilled);
  Submit(*b, 3, 1);
  EXPECT_EQ(adapter.FindOrder({1, 3}), nullptr);
}

TEST_F(PositionHoldsTest, FailedCashCheckDoesNotFreezeAnyContracts) {
  FundsTerms terms;
  terms.currency = model::Currency("CNY");
  terms.contract_multiplier = 1;
  terms.fees[1] = FeeRate{1000, 0};
  ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kSell, terms));
  Submit(*b, 1, 1);
  service.Run();
  EXPECT_EQ(Position().available_quantity(), 10);
  ASSERT_EQ(results.failures.size(), 1U);
  EXPECT_EQ(results.failures[0], FailureReason::kInsufficientFunds);
}

TEST_F(PositionHoldsTest, OpenFillAddsTodayWithoutChangingOutstandingHolds) {
  Submit(*a, 1, 7);
  Submit(*b, 2, 2, Offset::kOpen, Side::kBuy);
  service.Run();
  ASSERT_TRUE(adapter.Fill({{1, 2}, {"sim", "day", "open"}, 100, 2}));
  service.Run();
  EXPECT_EQ(Position().today_quantity, 6);
  EXPECT_EQ(Position().today_frozen, 1);
  EXPECT_EQ(Position().available_quantity(), 5);
  ASSERT_TRUE(adapter.Fill({{1, 1}, {"sim", "day", "close"}, 100, 7}));
  service.Run();
  EXPECT_EQ(Position().today_quantity, 5);
  EXPECT_EQ(Position().yesterday_quantity, 0);
  EXPECT_EQ(Position().available_quantity(), 5);
}

TEST_F(PositionHoldsTest, LateFillAfterCancelOnlyConsumesTheNewlyKnownQuantity) {
  Submit(*a, 1, 7);
  service.Run();
  a->CancelOrder({{1, 1}, 7, 2, 1});
  service.Run();
  EXPECT_EQ(Position().available_quantity(), 10);
  const TradeInfo late{{1, 1}, {"sim", "day", "late"}, 100, 2};
  adapter.EmitTrade(late);
  adapter.EmitTrade(late);
  EXPECT_EQ(Position().yesterday_quantity, 4);
  EXPECT_EQ(Position().yesterday_frozen, 0);
  EXPECT_EQ(Position().available_quantity(), 8);
}

TEST_F(PositionHoldsTest, SamePositionKeyInAnotherAccountIsIndependent) {
  ASSERT_TRUE(service.BindAccount(8, second_account));
  FundsTerms terms;
  terms.currency = model::Currency("CNY");
  terms.contract_multiplier = 1;
  terms.fees[1] = FeeRate{1, 0};
  ASSERT_TRUE(service.ConfigureFundsTerms(8, 42, Side::kSell, terms));
  const PositionInfo row{{42}, 1, 0, 0, 0, PositionCost{100, 10}};
  second_account.SetInitialState({terms.currency, 100, 90, 10, 0}, {&row, 1});
  service.Run();
  Submit(*a, 1, 7);
  b->SubmitOrder({{{1, 2}, 8, 3, 42, Side::kSell, Offset::kClose, 100, 1}});
  service.Run();
  EXPECT_TRUE(results.failures.empty());
  PositionInfo other;
  ASSERT_TRUE(b->QueryPosition({8, {42}}, other));
  EXPECT_EQ(other.today_frozen, 1);
  EXPECT_EQ(other.available_quantity(), 0);
  EXPECT_EQ(Position().available_quantity(), 3);
}

}  // namespace
}  // namespace yquant::trading
