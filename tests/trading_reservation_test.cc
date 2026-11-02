#include "yquant/trading/trading_client_factory.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/model/currency.h"
#include "yquant/trading/funds_terms.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {
namespace {

// Retains public request outcomes and checks that notifications follow return.
class ReservationResults final : public TradingListener {
 public:
  // TradingListener implementation.
  void OnRequestFailure(const RequestFailure& failure) override {
    EXPECT_FALSE(in_request);
    failures.push_back(failure);
    if (on_failure) {
      on_failure();
    }
  }
  void OnOrderReport(const OrderReport& report) override {
    EXPECT_FALSE(in_request);
    orders.push_back(report);
  }
  void OnCancelReport(const CancelReport&) override {
    ADD_FAILURE() << "Unexpected cancel report";
  }
  void OnTradeReport(const TradeReport&) override {
    ADD_FAILURE() << "Unexpected trade report";
  }

  bool in_request = false;
  std::vector<RequestFailure> failures;
  std::vector<OrderReport> orders;
  std::function<void()> on_failure;
};

// Counts real adapter calls and permits inspecting the ledger before sending.
class ReservationAdapter final : public ITradingAdapter {
 public:
  // ITradingAdapter implementation.
  void SetCallback(ITradingAdapterCallback& callback) override {
    counter.SetCallback(callback);
  }
  bool StartAccountSync(std::uint64_t id) override {
    return counter.StartAccountSync(id);
  }
  SendResult SubmitOrder(const SubmitOrderRequest& request) override {
    ++submits;
    if (inspect) {
      inspect();
    }
    return counter.SubmitOrder(request);
  }
  SendResult CancelOrder(const AdapterCancelOrderRequest& request) override {
    return counter.CancelOrder(request);
  }
  void Run() override { counter.Run(); }

  SimulatedTradingAdapter counter;
  int submits = 0;
  std::function<void()> inspect;
};

// Drives initialization, requests and queries through the normal client path.
class ReservationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(service.BindAccount(7, first));
    ASSERT_TRUE(service.BindAccount(8, second));
    TradingServiceRegistry registry;
    ASSERT_TRUE(registry.Register(3, service));
    client = TradingClientFactory(registry).Create(3);
    other = TradingClientFactory(registry).Create(3);
    ASSERT_NE(client, nullptr);
    ASSERT_NE(other, nullptr);
    terms.currency = model::Currency("CNY");
    terms.contract_multiplier = 1;
    terms.margin_rate = 0.7;
    for (auto& fee : terms.fees) {
      fee = FeeRate{};
    }
  }
  void Ready(double amount = 100) {
    Publish(first, 1, amount);
    ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kBuy, terms));
  }
  void Publish(ReservationAdapter& adapter, std::uint64_t id, double amount,
               model::Currency currency = model::Currency("CNY")) {
    adapter.counter.QueueFunds({id, FundsRefreshStarted{}});
    adapter.counter.QueueFunds({id, FundsInfo{currency, amount, amount, 0, 0}});
    adapter.counter.QueuePositions({id, PositionsRefreshStarted{}});
    adapter.counter.QueuePositions({id, PositionsRefreshCompleted{0}});
    adapter.counter.QueueAccountSync({id, AccountSyncResult::kComplete});
    service.Run();
  }
  SubmitOrderRequest Request(std::uint64_t sequence = 1) {
    return {{{100, sequence}, 7, 2, 42, Side::kBuy, Offset::kOpen, 100, 1}};
  }
  void Submit(const SubmitOrderRequest& request) {
    results.in_request = true;
    EXPECT_EQ(client->SubmitOrder(request, results), RequestResult::kDelivered);
    results.in_request = false;
  }
  FundsSnapshot Funds(std::uint64_t account = 7) {
    FundsSnapshot snapshot;
    EXPECT_TRUE(client->QueryFunds({account}, snapshot));
    return snapshot;
  }
  void ExpectFailure(FailureReason reason, int submits = 0) {
    service.Run();
    ASSERT_FALSE(results.failures.empty());
    EXPECT_EQ(results.failures.back().reason, reason);
    EXPECT_EQ(first.submits, submits);
  }

  ReservationAdapter first;
  ReservationAdapter second;
  ReservationResults results;
  TradingService service;
  std::unique_ptr<TradingClient> client;
  std::unique_ptr<TradingClient> other;
  FundsTerms terms;
};

TEST_F(ReservationTest, ReservesBeforeAdapterAndSharesTheAccountAcrossClients) {
  Ready();
  first.inspect = [&] {
    const auto snapshot = Funds();
    EXPECT_DOUBLE_EQ(snapshot.reserved, 70);
    EXPECT_DOUBLE_EQ(snapshot.available_for_orders(), 30);
  };
  Submit(Request());
  EXPECT_TRUE(results.orders.empty());
  EXPECT_TRUE(results.failures.empty());
  auto next = Request(2);
  next.order.strategy_id = 3;
  EXPECT_EQ(other->SubmitOrder(next, results), RequestResult::kDelivered);
  ExpectFailure(FailureReason::kInsufficientFunds, 1);
  EXPECT_FALSE(results.failures.back().snapshot);
  EXPECT_EQ(first.counter.FindOrder({100, 2}), nullptr);
  const auto snapshot = Funds();
  EXPECT_DOUBLE_EQ(snapshot.funds.available, 100);
  EXPECT_DOUBLE_EQ(snapshot.reserved, 70);
  EXPECT_DOUBLE_EQ(snapshot.available_for_orders(), 30);
}

TEST_F(ReservationTest, ExactFundsSufficeAndZeroFundsRejectPositiveCost) {
  terms.margin_rate = 1;
  Ready();
  Submit(Request());
  service.Run();
  EXPECT_EQ(first.submits, 1);
  EXPECT_DOUBLE_EQ(Funds().available_for_orders(), 0);
  Submit(Request(2));
  ExpectFailure(FailureReason::kInsufficientFunds, 1);
}

TEST_F(ReservationTest, MissingFundsNeverSendsOrCreatesAnOrder) {
  ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kBuy, terms));
  Submit(Request());
  ExpectFailure(FailureReason::kAccountNotReady);
  OrderSnapshot snapshot;
  EXPECT_FALSE(client->QueryOrder({{100, 1}, 7, 2}, snapshot));
  Publish(first, 1, 100);
  Submit(Request());
  service.Run();
  EXPECT_EQ(first.submits, 1);
}

TEST_F(ReservationTest, MissingParametersAndCurrencyMismatchPreserveFunds) {
  Publish(first, 1, 100);
  Submit(Request());
  ExpectFailure(FailureReason::kFundsParametersUnavailable);
  terms.currency = model::Currency("USD");
  ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kBuy, terms));
  Submit(Request());
  ExpectFailure(FailureReason::kCurrencyMismatch);
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
  terms.currency = model::Currency("CNY");
  terms.fees[0].reset();
  ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kBuy, terms));
  Submit(Request());
  ExpectFailure(FailureReason::kFundsParametersUnavailable);
}

TEST_F(ReservationTest, CalculatesOpeningMarginAndBothFeeComponents) {
  terms.contract_multiplier = 10;
  terms.margin_rate = 0.1;
  terms.fees[0] = FeeRate{2, 0.001};
  Ready(1000);
  auto request = Request();
  request.order.quantity = 2;
  Submit(request);
  EXPECT_DOUBLE_EQ(Funds().reserved, 206);
  EXPECT_DOUBLE_EQ(Funds().available_for_orders(), 794);
}

TEST_F(ReservationTest, CloseOffsetsReserveOnlyTheirConfiguredFees) {
  terms.margin_rate.reset();
  terms.fees[1] = FeeRate{3, 0.01};
  terms.fees[2] = FeeRate{7, 0.02};
  terms.fees[3] = FeeRate{11, 0.03};
  Ready(1000);
  double expected = 0;
  std::uint64_t sequence = 0;
  for (auto offset : {Offset::kClose, Offset::kCloseToday,
                      Offset::kCloseYesterday}) {
    auto request = Request(++sequence);
    request.order.offset = offset;
    request.order.quantity = 2;
    Submit(request);
    const auto& fee = *terms.fees[static_cast<std::size_t>(offset)];
    expected += fee.per_quantity * 2 + fee.per_notional * 200;
    EXPECT_DOUBLE_EQ(Funds().reserved, expected);
    EXPECT_EQ(first.submits, static_cast<int>(sequence));
  }
  Submit(Request(4));
  ExpectFailure(FailureReason::kFundsParametersUnavailable, 3);
}

TEST_F(ReservationTest, SideAndInstrumentParametersAreIndependent) {
  Ready();
  auto request = Request();
  request.order.side = Side::kSell;
  Submit(request);
  ExpectFailure(FailureReason::kFundsParametersUnavailable);
  auto sell = terms;
  sell.margin_rate = 0.2;
  ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kSell, sell));
  Submit(request);
  EXPECT_DOUBLE_EQ(Funds().reserved, 20);
  request.order.order_id.sequence = 2;
  request.order.instrument_id = 81;
  Submit(request);
  ExpectFailure(FailureReason::kFundsParametersUnavailable, 1);
}

TEST_F(ReservationTest, DefinitelyNotSentRollsBackExactlyBeforeNotification) {
  Ready();
  first.counter.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  Submit(Request());
  EXPECT_TRUE(results.failures.empty());
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
  EXPECT_DOUBLE_EQ(Funds().available_for_orders(), 100);
  ExpectFailure(FailureReason::kSendFailed, 1);
  ASSERT_TRUE(results.failures.back().snapshot);
  EXPECT_EQ(results.failures.back().snapshot->state, OrderState::kSendFailed);
  Submit(Request());
  ExpectFailure(FailureReason::kDuplicateOrder, 1);
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
  Submit(Request(2));
  service.Run();
  EXPECT_EQ(first.submits, 2);
  EXPECT_DOUBLE_EQ(Funds().reserved, 70);
}

TEST_F(ReservationTest, FailedSendsCannotAccumulateRoundingResidue) {
  terms.margin_rate = 1;
  Ready();
  auto initial = Request();
  initial.order.price = 70;
  Submit(initial);
  for (std::uint64_t sequence = 2; sequence < 102; ++sequence) {
    auto request = Request(sequence);
    request.order.price = 0.1;
    first.counter.SetNextSubmit({SendResult::kNotSent, std::nullopt});
    Submit(request);
    EXPECT_DOUBLE_EQ(Funds().reserved, 70);
  }
}

TEST_F(ReservationTest,
       FailedOrInvalidRequestsNeverChangeExistingReservations) {
  Ready();
  Submit(Request());
  Submit(Request());
  ExpectFailure(FailureReason::kDuplicateOrder, 1);
  auto invalid = Request(2);
  invalid.order.price = 0;
  Submit(invalid);
  ExpectFailure(FailureReason::kInvalidRequest, 1);
  invalid = Request(2);
  invalid.order.account_id = 99;
  Submit(invalid);
  ExpectFailure(FailureReason::kUnknownAccount, 1);
  EXPECT_DOUBLE_EQ(Funds().reserved, 70);
}

TEST_F(ReservationTest,
       LateSnapshotCannotReplaceLocalReservationState) {
  Ready();
  Submit(Request());
  Publish(first, 2, 30);
  FundsSnapshot output;
  ASSERT_TRUE(client->QueryFunds({7}, output));
  EXPECT_DOUBLE_EQ(output.funds.available, 100);
  EXPECT_DOUBLE_EQ(output.reserved, 70);
  Submit(Request(2));
  ExpectFailure(FailureReason::kInsufficientFunds, 1);
}

TEST_F(ReservationTest,
       CallbackCanRetryAfterSendFailureReleasedItsReservation) {
  Ready();
  first.counter.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  Submit(Request());
  results.on_failure = [&] { Submit(Request(2)); };
  service.Run();
  EXPECT_EQ(first.submits, 2);
  EXPECT_DOUBLE_EQ(Funds().reserved, 70);
  EXPECT_EQ(results.orders.size(), 1U);
}

TEST_F(ReservationTest, AnotherAccountKeepsItsOwnParametersAndReservations) {
  Ready();
  Publish(second, 1, 100);
  auto second_terms = terms;
  second_terms.margin_rate = 1;
  ASSERT_TRUE(service.ConfigureFundsTerms(8, 42, Side::kBuy, second_terms));
  Submit(Request());
  auto request = Request(2);
  request.order.account_id = 8;
  Submit(request);
  EXPECT_DOUBLE_EQ(Funds().reserved, 70);
  EXPECT_DOUBLE_EQ(Funds(8).reserved, 100);
  EXPECT_EQ(first.submits, 1);
  EXPECT_EQ(second.submits, 1);
}

TEST_F(ReservationTest, RejectsInvalidTermsAndUnrepresentableCalculations) {
  Publish(first, 1, 100);
  auto invalid = terms;
  invalid.contract_multiplier = 0;
  EXPECT_FALSE(service.ConfigureFundsTerms(7, 42, Side::kBuy, invalid));
  invalid = terms;
  invalid.margin_rate = -1;
  EXPECT_FALSE(service.ConfigureFundsTerms(7, 42, Side::kBuy, invalid));
  invalid = terms;
  invalid.fees[0] = FeeRate{-1, 0};
  EXPECT_FALSE(service.ConfigureFundsTerms(7, 42, Side::kBuy, invalid));
  terms.contract_multiplier = std::numeric_limits<double>::max();
  ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kBuy, terms));
  Submit(Request());
  ExpectFailure(FailureReason::kInvalidFundsRequirement);
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
}

TEST_F(ReservationTest, ZeroCashCannotFundPositiveRequirements) {
  Ready(0);
  Submit(Request());
  ExpectFailure(FailureReason::kInsufficientFunds);
  EXPECT_FALSE(service.RetryAccountSync(7));
  Submit(Request());
  ExpectFailure(FailureReason::kInsufficientFunds);
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
}

TEST_F(ReservationTest, NegativeCashCannotFundPositiveRequirements) {
  Ready(-10);
  Submit(Request());
  ExpectFailure(FailureReason::kInsufficientFunds);
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
}

TEST_F(ReservationTest, KnownZeroCostCanSendWithoutIncreasingReservations) {
  terms.margin_rate = 0;
  Ready(0);
  Submit(Request());
  service.Run();
  EXPECT_EQ(first.submits, 1);
  EXPECT_DOUBLE_EQ(Funds().reserved, 0);
  EXPECT_TRUE(results.failures.empty());
}

TEST_F(ReservationTest, InvalidSuppliedRatesNeverReplaceValidConfiguration) {
  Ready();
  for (double bad : {-1.0, std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN()}) {
    auto value = terms;
    value.contract_multiplier = bad;
    EXPECT_FALSE(service.ConfigureFundsTerms(7, 42, Side::kBuy, value));
    value = terms;
    value.margin_rate = bad;
    EXPECT_FALSE(service.ConfigureFundsTerms(7, 42, Side::kBuy, value));
    value = terms;
    value.fees[0] = FeeRate{bad, 0};
    EXPECT_FALSE(service.ConfigureFundsTerms(7, 42, Side::kBuy, value));
    value.fees[0] = FeeRate{0, bad};
    EXPECT_FALSE(service.ConfigureFundsTerms(7, 42, Side::kBuy, value));
  }
  EXPECT_FALSE(service.ConfigureFundsTerms(99, 42, Side::kBuy, terms));
  EXPECT_FALSE(service.ConfigureFundsTerms(7, 0, Side::kBuy, terms));
  EXPECT_FALSE(service.ConfigureFundsTerms(
      7, 42, static_cast<Side>(99), terms));
  auto invalid = terms;
  invalid.currency = model::Currency{};
  EXPECT_FALSE(service.ConfigureFundsTerms(7, 42, Side::kBuy, invalid));
  terms.margin_rate = 1;
  Submit(Request());
  EXPECT_DOUBLE_EQ(Funds().reserved, 70);
  EXPECT_FALSE(service.ConfigureFundsTerms(7, 42, Side::kBuy, terms));
}

TEST_F(ReservationTest, PositiveCostsCannotDisappearInArithmeticUnderflow) {
  terms.contract_multiplier = 1;
  terms.margin_rate = std::numeric_limits<double>::min();
  Ready();
  auto request = Request();
  request.order.price = std::numeric_limits<double>::min();
  Submit(request);
  ExpectFailure(FailureReason::kInvalidFundsRequirement);
}

TEST_F(ReservationTest, UnrepresentableReservationIncrementsCannotBeLost) {
  terms.margin_rate = 1;
  Ready(1e31);
  auto first_order = Request();
  first_order.order.price = 1e30;
  Submit(first_order);
  auto small_order = Request(2);
  small_order.order.price = 1;
  Submit(small_order);
  ExpectFailure(FailureReason::kInvalidFundsRequirement, 1);
}

}  // namespace
}  // namespace yquant::trading
