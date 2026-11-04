#include <functional>
#include <memory>

#include "gtest/gtest.h"
#include "quote_source_stub.h"
#include "yquant/market/market_client_factory.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client_factory.h"

namespace yquant::test {
namespace {

class Quotes final : public market::QuoteListener {
 public:
  void OnQuote(const model::Quote&) override {
    ++count;
    if (on_quote) on_quote();
  }
  int count = 0;
  std::function<void()> on_quote;
};

// Counts every callback so closing tests can detect unwanted late deliveries.
class Reports final : public trading::TradingListener {
 public:
  void OnRequestFailure(const trading::RequestFailure&) override { ++failures; }
  void OnOrderReport(const trading::OrderReport&) override {
    ++orders;
    if (on_order) on_order();
  }
  void OnCancelReport(const trading::CancelReport&) override { ++cancels; }
  void OnTradeReport(const trading::TradeReport&) override { ++trades; }
  int failures = 0, orders = 0, cancels = 0, trades = 0;
  std::function<void()> on_order;
};

TEST(ClientLifecycleTest, MarketBindingIsOnceOnlyAndClosedClientsCannotRebind) {
  QuoteSourceStub source;
  market::MarketService service(source);
  market::MarketServiceRegistry registry;
  registry.Register(1, service);
  market::MarketClientFactory factory(registry);
  Quotes first, second;
  auto client = factory.Create(1);
  ASSERT_TRUE(client);
  EXPECT_FALSE(client->Subscribe(42));
  EXPECT_FALSE(client->Unsubscribe(42));
  ASSERT_TRUE(client->BindListener(first));
  EXPECT_FALSE(client->BindListener(first));
  EXPECT_FALSE(client->BindListener(second));
  ASSERT_TRUE(client->Subscribe(42));
  source.SetQuote({42});
  service.Run();
  EXPECT_EQ(first.count, 1);
  EXPECT_EQ(second.count, 0);
  EXPECT_TRUE(client->Unsubscribe(42));
  EXPECT_FALSE(client->BindListener(second));
  EXPECT_TRUE(client->Close());
  EXPECT_TRUE(client->Close());
  EXPECT_FALSE(client->BindListener(first));
  EXPECT_FALSE(client->Subscribe(42));
  source.SetQuote({42});
  service.Run();
  EXPECT_EQ(first.count, 1);
  auto unbound = factory.Create(1);
  EXPECT_TRUE(unbound->Close());
  EXPECT_FALSE(unbound->BindListener(first));
}

TEST(ClientLifecycleTest, MarketDestructionRemovesAllSubscriptionsAndOnlyItsOwn) {
  QuoteSourceStub source;
  market::MarketService service(source);
  market::MarketServiceRegistry registry;
  ASSERT_TRUE(registry.Register(1, service));
  market::MarketClientFactory factory(registry);
  Quotes receiver;
  auto first = factory.Create(1, receiver);
  auto second = factory.Create(1, receiver);
  ASSERT_TRUE(first->Subscribe(42));
  ASSERT_TRUE(first->Subscribe(81));
  ASSERT_TRUE(second->Subscribe(42));
  source.SetQuote({42});
  service.Run();
  EXPECT_EQ(receiver.count, 2);
  first.reset();  // No manual Unsubscribe calls.
  source.SetQuote({81});
  service.Run();
  source.SetQuote({42});
  service.Run();
  EXPECT_EQ(receiver.count, 3);
  EXPECT_TRUE(second->Close());
  EXPECT_TRUE(second->Close());
  EXPECT_FALSE(second->Subscribe(42));
  source.SetQuote({42});
  service.Run();
  EXPECT_EQ(receiver.count, 3);
}

TEST(ClientLifecycleTest, MarketCloseDuringCallbackDoesNotInvalidateDispatch) {
  QuoteSourceStub source;
  market::MarketService service(source);
  market::MarketServiceRegistry registry;
  registry.Register(1, service);
  Quotes first, second;
  market::MarketClientFactory factory(registry);
  auto a = factory.Create(1, first);
  auto b = factory.Create(1, second);
  a->Subscribe(42);
  b->Subscribe(42);
  first.on_quote = [&] {
    EXPECT_FALSE(a->CanClose());
    EXPECT_FALSE(a->Close());
    EXPECT_FALSE(a->Unsubscribe(42));
    auto unused = factory.Create(1, second);
    EXPECT_FALSE(unused->Subscribe(42));
    EXPECT_TRUE(unused->Close());
  };
  source.SetQuote({42});
  service.Run();
  EXPECT_EQ(first.count, 1);
  EXPECT_EQ(second.count, 1);
  ASSERT_TRUE(a->Close());
  source.SetQuote({42});
  service.Run();
  EXPECT_EQ(first.count, 1);
  EXPECT_EQ(second.count, 2);
}

class TradingClientLifecycleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(service.BindAccount(7, adapter));
    trading::FundsTerms terms;
    terms.currency = model::Currency("CNY");
    terms.contract_multiplier = 1;
    terms.margin_rate = 0.1;
    for (auto& fee : terms.fees) fee = trading::FeeRate{1, 0};
    ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, trading::Side::kBuy, terms));
    adapter.SetInitialState({terms.currency, 1000, 1000, 0, 0}, {});
    service.Run();
    ASSERT_TRUE(registry.Register(1, service));
  }
  trading::SimulatedTradingAdapter adapter;
  trading::TradingService service;
  trading::TradingServiceRegistry registry;
  trading::TradingClientFactory factory{registry};
  trading::SubmitOrderRequest request{{{10, 1}, 7, 11, 42,
      trading::Side::kBuy, trading::Offset::kOpen, 100, 2}};
};

TEST_F(TradingClientLifecycleTest, BindingIsOnceOnlyAndClosedClientsCannotSend) {
  Reports first, second;
  auto client = factory.Create(1);
  EXPECT_EQ(client->SubmitOrder(request), trading::RequestResult::kUnavailable);
  ASSERT_TRUE(client->BindListener(first));
  EXPECT_FALSE(client->BindListener(second));
  ASSERT_EQ(client->SubmitOrder(request), trading::RequestResult::kDelivered);
  service.Run();
  EXPECT_EQ(first.orders, 1);
  EXPECT_EQ(second.orders, 0);
  EXPECT_TRUE(client->Close());
  EXPECT_TRUE(client->Close());
  EXPECT_FALSE(client->BindListener(second));
  EXPECT_EQ(client->SubmitOrder(request), trading::RequestResult::kUnavailable);
  EXPECT_EQ(client->CancelOrder({request.order.order_id, 7, 11, 1}),
            trading::RequestResult::kUnavailable);
  EXPECT_FALSE(client->AdoptWorkingOrders({10, 7, 11}, {}));
  trading::OrderSnapshot snapshot;
  EXPECT_FALSE(client->QueryOrder({request.order.order_id, 7, 11}, snapshot));
}

TEST_F(TradingClientLifecycleTest, DestructionDropsQueuedReportsAndLateFillsStillBook) {
  auto receiver = std::make_unique<Reports>();
  auto client = factory.Create(1, *receiver);
  ASSERT_EQ(client->SubmitOrder(request), trading::RequestResult::kDelivered);
  ASSERT_EQ(client->SubmitOrder(request), trading::RequestResult::kDelivered); // Queued duplicate failure.
  client.reset();
  receiver.reset();
  service.Run();  // A queued accept and failure must not touch the destroyed receiver.
  trading::OrderSnapshot snapshot;
  ASSERT_TRUE(service.QueryOrder({request.order.order_id, 7, 11}, snapshot));
  EXPECT_EQ(snapshot.state, trading::OrderState::kAccepted);
  ASSERT_TRUE(adapter.Fill({request.order.order_id, {"test", "day", "late"}, 100, 1}));
  service.Run();
  trading::FundsSnapshot funds;
  ASSERT_TRUE(service.QueryFunds({7}, funds));
  EXPECT_DOUBLE_EQ(funds.commission, 1);
  EXPECT_DOUBLE_EQ(funds.funds.margin, 10);
  EXPECT_DOUBLE_EQ(funds.reserved, 11);
  trading::PositionInfo position;
  ASSERT_TRUE(service.QueryPosition({7, {42, trading::PositionSide::kLong}}, position));
  EXPECT_EQ(position.today_quantity, 1);
}

TEST_F(TradingClientLifecycleTest, SharedReceiverDoesNotMergeClientRegistrations) {
  Reports receiver;
  auto first = factory.Create(1, receiver);
  auto second = factory.Create(1, receiver);
  auto other = request;
  other.order.order_id.sequence = 2;
  first->SubmitOrder(request);
  second->SubmitOrder(other);
  first.reset();
  service.Run();
  EXPECT_EQ(receiver.orders, 1);
  adapter.EmitTrade({request.order.order_id, {"test", "day", "first"}, 100, 1});
  EXPECT_EQ(receiver.trades, 0);
  adapter.EmitTrade({other.order.order_id, {"test", "day", "second"}, 100, 1});
  EXPECT_EQ(receiver.trades, 1);
}

TEST_F(TradingClientLifecycleTest, CloseDuringCallbackFailsWithoutDroppingLaterReports) {
  Reports receiver;
  auto client = factory.Create(1, receiver);
  receiver.on_order = [&] {
    EXPECT_FALSE(client->CanClose());
    EXPECT_FALSE(client->Close());
    auto unused = factory.Create(1, receiver);
    EXPECT_TRUE(unused->Close());
  };
  client->SubmitOrder(request);
  service.Run();
  ASSERT_EQ(receiver.orders, 1);
  ASSERT_TRUE(adapter.Fill({request.order.order_id, {"test", "day", "one"}, 100, 1}));
  service.Run();
  EXPECT_EQ(receiver.trades, 1);
  ASSERT_TRUE(client->Close());
  adapter.EmitTrade({request.order.order_id, {"test", "day", "two"}, 100, 1});
  EXPECT_EQ(receiver.trades, 1);
}

TEST_F(TradingClientLifecycleTest, CancelAndAdoptionUseTheBoundReceiver) {
  Reports original, replacement;
  auto first = factory.Create(1, original);
  first->SubmitOrder(request);
  service.Run();
  ASSERT_TRUE(first->Close());
  auto second = factory.Create(1, replacement);
  std::vector<trading::OrderSnapshot> active;
  std::uint64_t last = 0;
  ASSERT_TRUE(second->QueryWorkingOrders({10, 7, 11}, active, last));
  ASSERT_EQ(active.size(), 1);
  ASSERT_TRUE(second->AdoptWorkingOrders({10, 7, 11}, active));
  ASSERT_EQ(second->CancelOrder({request.order.order_id, 7, 11, 1}),
            trading::RequestResult::kDelivered);
  service.Run();
  EXPECT_EQ(replacement.cancels, 1);
  EXPECT_EQ(original.cancels, 0);
  EXPECT_EQ(original.orders, 1);
}

}  // namespace
}  // namespace yquant::test
