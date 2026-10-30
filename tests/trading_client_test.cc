#include "yquant/trading/trading_client_factory.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/trading/order.h"
#include "yquant/trading/order_id_generator.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_listener.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {
namespace {

// Retains full reports so tests inspect public results after calls return.
class ClientListener final : public TradingListener {
 public:
  // TradingListener implementation.
  void OnTradeReport(const TradeReport&) override {
    ADD_FAILURE() << "Unexpected trade report";
  }
  void OnOrderReport(const OrderReport& report) override {
    EXPECT_FALSE(in_request);
    reports.push_back(report);
  }
  void OnRequestFailure(const RequestFailure& failure) override {
    EXPECT_FALSE(in_request);
    failures.push_back(failure);
  }
  void OnCancelReport(const CancelReport&) override {
    ADD_FAILURE() << "Unexpected cancellation report";
  }

  bool in_request = false;
  std::vector<OrderReport> reports;
  std::vector<RequestFailure> failures;
};

TEST(TradingClientTest, RegistryAndFactoryUseInjectedConcreteServices) {
  TradingService first;
  TradingService second;
  TradingServiceRegistry registry;
  ASSERT_TRUE(registry.Register(1, first));
  EXPECT_FALSE(registry.Register(1, second));
  EXPECT_EQ(registry.Find(1), &first);
  EXPECT_EQ(registry.Find(2), nullptr);
  TradingClientFactory factory(registry);
  EXPECT_NE(factory.Create(1), nullptr);
  EXPECT_EQ(factory.Create(2), nullptr);
  TradingServiceRegistry empty;
  TradingClientFactory other(empty);
  EXPECT_EQ(other.Create(1), nullptr);
}

TEST(TradingClientTest, ClientAndReportsOutliveFactoryRegistryAndClient) {
  SimulatedTradingAdapter adapter;
  ClientListener listener;
  TradingService service;
  ASSERT_TRUE(service.BindAccount(7, adapter));
  std::unique_ptr<TradingClient> client;
  {
    TradingServiceRegistry registry;
    ASSERT_TRUE(registry.Register(3, service));
    TradingClientFactory factory(registry);
    client = factory.Create(3);
  }
  ASSERT_NE(client, nullptr);
  SubmitOrderRequest request{{{100, 1}, 7, 2, 42, Side::kSell,
                             Offset::kClose, 99, 5}};
  const auto original = request.order;
  adapter.SetNextSubmit(
      {SendResult::kSent, AdapterOrderState::kAccepted});
  listener.in_request = true;
  EXPECT_EQ(client->SubmitOrder(request, listener), RequestResult::kDelivered);
  listener.in_request = false;
  EXPECT_TRUE(listener.reports.empty());
  request.order.price = 999;
  client.reset();
  service.Run();
  ASSERT_EQ(listener.reports.size(), 1U);
  EXPECT_EQ(listener.reports[0].snapshot.order, original);
}

TEST(TradingClientTest, SharedGeneratorCoversStrategiesAccountsAndTargets) {
  SimulatedTradingAdapter account_a;
  SimulatedTradingAdapter account_b;
  SimulatedTradingAdapter second_target_account;
  ClientListener listener;
  TradingService first;
  TradingService second;
  ASSERT_TRUE(first.BindAccount(7, account_a));
  ASSERT_TRUE(first.BindAccount(8, account_b));
  ASSERT_TRUE(second.BindAccount(7, second_target_account));
  TradingServiceRegistry registry;
  ASSERT_TRUE(registry.Register(1, first));
  ASSERT_TRUE(registry.Register(2, second));
  TradingClientFactory factory(registry);
  auto first_client = factory.Create(1);
  auto second_client = factory.Create(2);
  ASSERT_NE(first_client, nullptr);
  ASSERT_NE(second_client, nullptr);
  OrderIdGenerator ids(100);
  const auto first_id = ids.Next();
  const auto second_id = ids.Next();
  const auto third_id = ids.Next();
  ASSERT_TRUE(first_id && second_id && third_id);
  EXPECT_EQ(first_client->SubmitOrder(
                {{*first_id, 7, 2, 42, Side::kBuy, Offset::kOpen, 100, 1}},
                listener), RequestResult::kDelivered);
  EXPECT_EQ(first_client->SubmitOrder(
                {{*second_id, 8, 3, 81, Side::kSell, Offset::kClose, 200, 2}},
                listener), RequestResult::kDelivered);
  EXPECT_EQ(second_client->SubmitOrder(
                {{*third_id, 7, 4, 42, Side::kSell, Offset::kOpen, 101, 3}},
                listener), RequestResult::kDelivered);
  first.Run();
  second.Run();
  ASSERT_EQ(listener.reports.size(), 3U);
  EXPECT_EQ(listener.reports[0].snapshot.order.order_id, (OrderId{100, 1}));
  EXPECT_EQ(listener.reports[1].snapshot.order.order_id, (OrderId{100, 2}));
  EXPECT_EQ(listener.reports[2].snapshot.order.order_id, (OrderId{100, 3}));
  ASSERT_NE(account_a.FindOrder(*first_id), nullptr);
  ASSERT_NE(account_b.FindOrder(*second_id), nullptr);
  ASSERT_NE(second_target_account.FindOrder(*third_id), nullptr);
  EXPECT_EQ(account_b.FindOrder(*second_id)->strategy_id, 3U);
  EXPECT_EQ(second_target_account.FindOrder(*third_id)->account_id, 7U);
}

TEST(TradingClientTest, DuplicateInvocationDoesNotReplaceOriginalListener) {
  SimulatedTradingAdapter adapter;
  ClientListener original;
  ClientListener duplicate;
  ClientListener other_duplicate;
  TradingService service;
  ASSERT_TRUE(service.BindAccount(7, adapter));
  TradingServiceRegistry registry;
  ASSERT_TRUE(registry.Register(1, service));
  TradingClientFactory factory(registry);
  auto client = factory.Create(1);
  auto other_client = factory.Create(1);
  ASSERT_NE(client, nullptr);
  ASSERT_NE(other_client, nullptr);
  SubmitOrderRequest request{{{100, 1}, 7, 2, 42, Side::kBuy,
                             Offset::kOpen, 100, 1}};
  EXPECT_EQ(client->SubmitOrder(request, original), RequestResult::kDelivered);
  EXPECT_EQ(client->SubmitOrder(request, duplicate), RequestResult::kDelivered);
  EXPECT_EQ(other_client->SubmitOrder(request, other_duplicate),
            RequestResult::kDelivered);
  service.Run();
  ASSERT_EQ(original.reports.size(), 1U);
  ASSERT_EQ(duplicate.failures.size(), 1U);
  ASSERT_EQ(other_duplicate.failures.size(), 1U);
  EXPECT_TRUE(original.failures.empty());
  EXPECT_TRUE(duplicate.reports.empty());
  EXPECT_TRUE(other_duplicate.reports.empty());
  EXPECT_EQ(duplicate.failures[0].reason, FailureReason::kDuplicateOrder);
  EXPECT_EQ(other_duplicate.failures[0].reason, FailureReason::kDuplicateOrder);
  EXPECT_EQ(duplicate.failures[0].order_id, request.order.order_id);
  EXPECT_EQ(other_duplicate.failures[0].order_id, request.order.order_id);
  EXPECT_FALSE(duplicate.failures[0].snapshot);
  EXPECT_FALSE(other_duplicate.failures[0].snapshot);
}

}  // namespace
}  // namespace yquant::trading
