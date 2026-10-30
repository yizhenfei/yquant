#include "yquant/trading/trading_client_factory.h"

#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/trading/order.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_listener.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {
namespace {

// Retains the snapshots delivered to the order's actual listener.
class QueryResults final : public TradingListener {
 public:
  // TradingListener implementation.
  void OnTradeReport(const TradeReport&) override {
    ADD_FAILURE() << "Unexpected trade report";
  }
  void OnRequestFailure(const RequestFailure&) override {
    ADD_FAILURE() << "Unexpected request failure";
  }
  void OnOrderReport(const OrderReport& report) override {
    snapshots.push_back(report.snapshot);
  }
  void OnCancelReport(const CancelReport&) override {
    ADD_FAILURE() << "Unexpected cancellation report";
  }

  std::vector<OrderSnapshot> snapshots;
};

// Returns a client whose registry and factory are no longer needed.
std::unique_ptr<TradingClient> MakeClient(TradingService& service) {
  TradingServiceRegistry registry;
  EXPECT_TRUE(registry.Register(3, service));
  return TradingClientFactory(registry).Create(3);
}

TEST(OrderQueryTest, ReadsTheOrderBeforeAndAfterItsCounterReport) {
  SimulatedTradingAdapter adapter;
  QueryResults listener;
  TradingService service;
  ASSERT_TRUE(service.BindAccount(7, adapter));
  auto client = MakeClient(service);
  ASSERT_NE(client, nullptr);
  const SubmitOrderRequest request{
      {{100, 1}, 7, 2, 42, Side::kBuy, Offset::kOpen, 100.5, 5}};
  OrderSnapshot snapshot;
  EXPECT_FALSE(client->QueryOrder({request.order.order_id, 7, 2}, snapshot));
  EXPECT_EQ(client->SubmitOrder(request, listener), RequestResult::kDelivered);
  ASSERT_TRUE(client->QueryOrder({request.order.order_id, 7, 2}, snapshot));
  EXPECT_EQ(snapshot.order, request.order);
  EXPECT_EQ(snapshot.state, OrderState::kSent);
  EXPECT_EQ(snapshot.filled_quantity, 0);
  EXPECT_TRUE(listener.snapshots.empty());
  service.Run();
  ASSERT_TRUE(client->QueryOrder({request.order.order_id, 7, 2}, snapshot));
  EXPECT_EQ(snapshot.state, OrderState::kAccepted);
  ASSERT_EQ(listener.snapshots.size(), 1U);
  EXPECT_EQ(snapshot.version, listener.snapshots[0].version);
}

TEST(OrderQueryTest, FailedQueriesPreserveTheEntireOutputValue) {
  SimulatedTradingAdapter adapter;
  QueryResults listener;
  TradingService service;
  ASSERT_TRUE(service.BindAccount(7, adapter));
  auto client = MakeClient(service);
  ASSERT_NE(client, nullptr);
  EXPECT_EQ(client->SubmitOrder(
                {{{100, 1}, 7, 2, 42, Side::kBuy, Offset::kOpen, 100, 5}},
                listener), RequestResult::kDelivered);
  const OrderSnapshot original{
      {{200, 99}, 8, 3, 81, Side::kSell, Offset::kClose, 9, 7},
      OrderState::kCanceled, 4, 88};
  for (const QueryOrderRequest query : {
           QueryOrderRequest{{100, 2}, 7, 2},
           QueryOrderRequest{{100, 1}, 8, 2},
           QueryOrderRequest{{100, 1}, 7, 3}}) {
    auto output = original;
    EXPECT_FALSE(client->QueryOrder(query, output));
    EXPECT_EQ(output.order, original.order);
    EXPECT_EQ(output.state, original.state);
    EXPECT_EQ(output.filled_quantity, original.filled_quantity);
    EXPECT_EQ(output.version, original.version);
  }
}

}  // namespace
}  // namespace yquant::trading
