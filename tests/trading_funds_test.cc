#include "yquant/trading/trading_client_factory.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/model/currency.h"
#include "yquant/trading/funds.h"
#include "yquant/trading/order.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_listener.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {
namespace {

const FundsInfo kInitial{model::Currency("CNY"), 1000, 700, 200, 100};
const FundsInfo kUpdated{model::Currency("CNY"), 900, 650, 150, 100};
const FundsInfo kZero{model::Currency("USD"), 0, 0, 0, 0};

// Observes account state only through clients sharing the same local service.
class FundsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(service.BindAccount(7, first));
    ASSERT_TRUE(service.BindAccount(8, second));
    TradingServiceRegistry registry;
    ASSERT_TRUE(registry.Register(3, service));
    TradingClientFactory factory(registry);
    client = factory.Create(3);
    observer = factory.Create(3);
    ASSERT_NE(client, nullptr);
    ASSERT_NE(observer, nullptr);
  }
  void Begin(SimulatedTradingAdapter& adapter, std::uint64_t id) {
    adapter.QueueFunds({id, FundsRefreshStarted{}});
  }
  void Complete(SimulatedTradingAdapter& adapter, std::uint64_t id,
                const FundsInfo& funds) {
    adapter.QueueFunds({id, funds});
  }
  void Publish(SimulatedTradingAdapter& adapter, std::uint64_t id,
               const FundsInfo& funds) {
    Begin(adapter, id);
    Complete(adapter, id, funds);
    service.Run();
  }
  FundsSnapshot Snapshot(std::uint64_t account_id = 7) {
    FundsSnapshot snapshot;
    EXPECT_TRUE(client->QueryFunds({account_id}, snapshot));
    return snapshot;
  }
  void ExpectUnavailable(std::uint64_t account_id = 7) {
    const FundsSnapshot sentinel{kUpdated, 999};
    auto output = sentinel;
    EXPECT_FALSE(client->QueryFunds({account_id}, output));
    EXPECT_EQ(output, sentinel);
  }

  SimulatedTradingAdapter first;
  SimulatedTradingAdapter second;
  TradingService service;
  std::unique_ptr<TradingClient> client;
  std::unique_ptr<TradingClient> observer;
};

TEST_F(FundsTest, UnknownAndUninitializedAccountsDoNotChangeOutput) {
  ExpectUnavailable();
  ExpectUnavailable(8);
  ExpectUnavailable(99);
  Begin(first, 1);
  service.Run();
  ExpectUnavailable();
}

TEST_F(FundsTest, PublishesOnlyCompleteSnapshotsIncludingValidZeroFunds) {
  Begin(first, 1);
  service.Run();
  Complete(first, 1, kZero);
  ExpectUnavailable();
  service.Run();
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kZero, 1}));
  ExpectUnavailable(8);
  FundsSnapshot shared;
  EXPECT_TRUE(observer->QueryFunds({7}, shared));
  EXPECT_EQ(shared, Snapshot());
}

TEST_F(FundsTest, RefreshKeepsThePreviousCompleteViewUntilReplacement) {
  Publish(first, 1, kInitial);
  const auto old = Snapshot();
  Begin(first, 2);
  service.Run();
  EXPECT_EQ(Snapshot(), old);
  Complete(first, 2, kUpdated);
  EXPECT_EQ(Snapshot(), old);
  service.Run();
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kUpdated, 2}));
  EXPECT_EQ(old, (FundsSnapshot{kInitial, 1}));
}

TEST_F(FundsTest,
       AccountsAndCurrenciesRemainIndependentDuringInterleavedUpdates) {
  Begin(first, 1);
  Begin(second, 1);
  Complete(second, 1, kZero);
  service.Run();
  ExpectUnavailable();
  EXPECT_EQ(Snapshot(8), (FundsSnapshot{kZero, 1}));
  Complete(first, 1, kInitial);
  service.Run();
  Publish(first, 2, kUpdated);
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kUpdated, 2}));
  EXPECT_EQ(Snapshot(8), (FundsSnapshot{kZero, 1}));
  Begin(second, 2);
  second.QueueFunds({2, FundsRefreshFailed{}});
  service.Run();
  ExpectUnavailable(8);
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kUpdated, 2}));
}

TEST_F(FundsTest, InitialFailureStaysUnavailableUntilANewRefreshCompletes) {
  Begin(first, 1);
  first.QueueFunds({1, FundsRefreshFailed{}});
  ::testing::internal::CaptureStderr();
  service.Run();
  const auto diagnostic = ::testing::internal::GetCapturedStderr();
  EXPECT_NE(diagnostic.find("funds_refresh_failed account=7 refresh=1"),
            std::string::npos);
  ExpectUnavailable();
  Complete(first, 1, kInitial);
  service.Run();
  ExpectUnavailable();
  Publish(first, 2, kInitial);
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kInitial, 2}));
}

TEST_F(FundsTest,
       InterruptedRefreshInvalidatesQueryWithoutPublishingZeroFunds) {
  Publish(first, 1, kInitial);
  Begin(first, 2);
  service.Run();
  EXPECT_EQ(Snapshot().funds, kInitial);
  first.QueueFunds({2, FundsRefreshFailed{}});
  service.Run();
  ExpectUnavailable();
  Begin(first, 3);
  service.Run();
  ExpectUnavailable();
  Complete(first, 2, kZero);
  service.Run();
  ExpectUnavailable();
  Complete(first, 3, kUpdated);
  service.Run();
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kUpdated, 3}));
}

TEST_F(FundsTest, NewRefreshSupersedesUnfinishedOneAndIgnoresItsLateEvents) {
  Publish(first, 1, kInitial);
  Begin(first, 2);
  Begin(first, 3);
  Complete(first, 2, kZero);
  first.QueueFunds({2, FundsRefreshFailed{}});
  service.Run();
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kInitial, 1}));
  Complete(first, 3, kUpdated);
  service.Run();
  Begin(first, 1);
  Begin(first, 3);
  Complete(first, 1, kZero);
  Complete(first, 3, kZero);
  first.QueueFunds({3, FundsRefreshFailed{}});
  service.Run();
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kUpdated, 3}));
}

TEST_F(FundsTest, UnstartedAndZeroIdsCannotPublishOrAdvanceTheRefresh) {
  Complete(first, 50, kInitial);
  first.QueueFunds({51, FundsRefreshFailed{}});
  Begin(first, 0);
  Complete(first, 0, kInitial);
  service.Run();
  ExpectUnavailable();
  Publish(first, 1, kInitial);
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kInitial, 1}));
  Complete(first, 99, kZero);
  service.Run();
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kInitial, 1}));
}

TEST_F(FundsTest, DuplicateStartDoesNotDiscardThePendingResult) {
  Begin(first, 1);
  Begin(first, 1);
  Complete(first, 1, kInitial);
  service.Run();
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kInitial, 1}));
}

TEST_F(FundsTest, RejectsNonfiniteAmountsAndMalformedCurrencies) {
  Publish(first, 1, kInitial);
  std::vector<FundsInfo> invalid;
  for (auto field : {&FundsInfo::balance, &FundsInfo::available,
                     &FundsInfo::margin, &FundsInfo::frozen_margin}) {
    for (double number : {std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()}) {
      auto value = kInitial;
      value.*field = number;
      invalid.push_back(value);
    }
  }
  for (auto currency : {model::Currency{}, model::Currency("cny"),
                        model::Currency("123")}) {
    auto value = kInitial;
    value.currency = currency;
    invalid.push_back(value);
  }
  std::uint64_t id = 1;
  for (const auto& value : invalid) {
    ++id;
    Publish(first, id, value);
    ExpectUnavailable();
    Complete(first, id, kInitial);
    service.Run();
    ExpectUnavailable();
  }
  Publish(first, ++id, kUpdated);
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kUpdated, id}));
}

TEST_F(FundsTest, PreservesFiniteSignedAmountsWithoutInferringRiskConstraints) {
  const FundsInfo deficit{model::Currency("CNY"), -10, -30, 20, 0};
  Publish(first, 1, deficit);
  EXPECT_EQ(Snapshot().funds, deficit);
}

TEST_F(FundsTest, QueuedReportOwnsTheAmountsAndCurrency) {
  Begin(first, 1);
  AdapterFundsReport report{1, kInitial};
  first.QueueFunds(report);
  report.event = kZero;
  service.Run();
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kInitial, 1}));
}

TEST_F(FundsTest, MaximumRefreshIdDoesNotPermitWraparoundOrStaleOverwrite) {
  constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
  Publish(first, kMax, kInitial);
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kInitial, kMax}));
  Begin(first, 1);
  Complete(first, 1, kZero);
  first.QueueFunds({1, FundsRefreshFailed{}});
  service.Run();
  EXPECT_EQ(Snapshot(), (FundsSnapshot{kInitial, kMax}));
}

TEST_F(FundsTest, FundsReportsRespectTheBatchLimitAndAllowOtherAccountsToRun) {
  for (std::uint64_t id = 1; id <= 65; ++id) {
    Begin(first, id);
    Complete(first, id, kInitial);
  }
  Begin(second, 1);
  Complete(second, 1, kZero);
  service.Run();
  EXPECT_EQ(Snapshot().refresh_id, 32U);
  EXPECT_EQ(Snapshot(8), (FundsSnapshot{kZero, 1}));
  service.Run();
  EXPECT_EQ(Snapshot().refresh_id, 64U);
  service.Run();
  EXPECT_EQ(Snapshot().refresh_id, 65U);
}

// Checks that account data remains queryable inside ordinary order callbacks.
class FundsOrderListener final : public TradingListener {
 public:
  FundsOrderListener(TradingClient& client, TradingService& service)
      : client(client), service(service) {}

  // TradingListener implementation.
  void OnRequestFailure(const RequestFailure&) override {
    ADD_FAILURE() << "Unexpected order request failure";
  }
  void OnOrderReport(const OrderReport&) override {
    FundsSnapshot snapshot;
    EXPECT_TRUE(client.QueryFunds({7}, snapshot));
    EXPECT_EQ(snapshot, (FundsSnapshot{kInitial, 1}));
    service.Run();
    ++orders;
  }
  void OnCancelReport(const CancelReport&) override {
    ADD_FAILURE() << "Unexpected cancellation report";
  }
  void OnTradeReport(const TradeReport&) override {
    ADD_FAILURE() << "Unexpected trade report";
  }

  TradingClient& client;
  TradingService& service;
  int orders = 0;
};

TEST_F(FundsTest, FundsQueriesInOrderCallbacksAndFailuresPreserveOrderState) {
  FundsOrderListener listener(*client, service);
  Begin(first, 1);
  Complete(first, 1, kInitial);
  ASSERT_EQ(client->SubmitOrder(
      {{{100, 1}, 7, 2, 42, Side::kBuy, Offset::kOpen, 100, 5}}, listener),
      RequestResult::kDelivered);
  service.Run();
  ASSERT_EQ(listener.orders, 1);
  OrderSnapshot accepted;
  ASSERT_TRUE(client->QueryOrder({{100, 1}, 7, 2}, accepted));
  EXPECT_EQ(accepted.state, OrderState::kAccepted);
  Begin(first, 2);
  first.QueueFunds({2, FundsRefreshFailed{}});
  service.Run();
  ExpectUnavailable();
  OrderSnapshot after;
  ASSERT_TRUE(client->QueryOrder({{100, 1}, 7, 2}, after));
  EXPECT_EQ(after.order, accepted.order);
  EXPECT_EQ(after.state, accepted.state);
  EXPECT_EQ(after.filled_quantity, accepted.filled_quantity);
  EXPECT_EQ(after.version, accepted.version);
  EXPECT_EQ(listener.orders, 1);
}

TEST(FundsAdapterTest, PreboundFundsEventsWaitForTheirAccountCallback) {
  SimulatedTradingAdapter adapter;
  adapter.QueueFunds({1, FundsRefreshStarted{}});
  adapter.QueueFunds({1, kInitial});
  adapter.Run();
  TradingService service;
  ASSERT_TRUE(service.BindAccount(7, adapter));
  TradingServiceRegistry registry;
  ASSERT_TRUE(registry.Register(3, service));
  auto client = TradingClientFactory(registry).Create(3);
  ASSERT_NE(client, nullptr);
  FundsSnapshot snapshot;
  EXPECT_FALSE(client->QueryFunds({7}, snapshot));
  service.Run();
  EXPECT_TRUE(client->QueryFunds({7}, snapshot));
  EXPECT_EQ(snapshot, (FundsSnapshot{kInitial, 1}));
}

}  // namespace
}  // namespace yquant::trading
