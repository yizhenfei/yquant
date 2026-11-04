#include "yquant/strategy/strategy_service.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/market/market_client_factory.h"
#include "yquant/market/market_service.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::strategy {
namespace {

class Source final : public market::QuoteSource {
 public:
  std::vector<model::Quote> quotes;
  std::size_t index = 0;
  // market::QuoteSource implementation.
  bool TryRead(model::Quote& q) override {
    if (index == quotes.size()) return false;
    q = quotes[index++];
    return true;
  }
};

class Counter final : public trading::ITradingAdapter {
 public:
  trading::SimulatedTradingAdapter sim;
  std::vector<trading::OrderInfo> orders;
  std::function<void()> on_submit;
  // trading::ITradingAdapter implementation.
  void SetCallback(trading::ITradingAdapterCallback& c) override { sim.SetCallback(c); }
  bool StartAccountSync(std::uint64_t id) override { return sim.StartAccountSync(id); }
  trading::SendResult SubmitOrder(const trading::SubmitOrderRequest& r) override {
    orders.push_back(r.order);
    if (on_submit) on_submit();
    return sim.SubmitOrder(r);
  }
  trading::SendResult CancelOrder(const trading::AdapterCancelOrderRequest& r) override {
    return sim.CancelOrder(r);
  }
  void Run() override { sim.Run(); }
};

class StrategyInstancesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char temp[] = "/tmp/yquant-instances-XXXXXX";
    ASSERT_NE(mkdtemp(temp), nullptr);
    root = temp;
    ASSERT_TRUE(trading.BindAccount(7, counter));
    trading::FundsTerms terms;
    terms.currency = model::Currency("CNY");
    terms.contract_multiplier = 1;
    terms.margin_rate = 0.1;
    for (auto& fee : terms.fees) fee = trading::FeeRate{};
    for (auto instrument : {42, 81}) {
      ASSERT_TRUE(trading.ConfigureFundsTerms(7, instrument, trading::Side::kBuy, terms));
    }
    counter.sim.SetInitialState({terms.currency,1000,1000,0,0}, {});
    trading.Run();
    ASSERT_TRUE(registry.Register(1,trading));
    ASSERT_TRUE(markets.Register(1, market));
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  StrategyConfig Config(std::uint64_t id, std::uint32_t instrument = 42) {
    return {FIXED_PLUGIN,id,7,
            {{"instrument_id",std::to_string(instrument)}, {"price",std::to_string(80 + id)}, {"marker",Marker(id)}}};
  }
  std::string Marker(std::uint64_t id) { return (root / std::to_string(id)).string(); }
  std::string Reports(std::uint64_t id) {
    std::ifstream file(Marker(id) + ".orders");
    return {std::istreambuf_iterator<char>(file), {}};
  }
  void Publish(std::uint32_t instrument) {
    source.quotes.push_back({instrument,100,102,1,1,1});
    market.Run();
  }

  std::filesystem::path root;
  Source source;
  market::MarketService market{source};
  Counter counter;
  trading::TradingService trading;
  market::MarketServiceRegistry markets;
  trading::TradingServiceRegistry registry;
  market::MarketClientFactory market_factory{markets};
  trading::TradingClientFactory trading_factory{registry};
  std::string error;
};

TEST_F(StrategyInstancesTest, SameAndDifferentInstrumentsShareOnlyTheServiceSequence) {
  StrategyService group(10,market_factory,1,trading_factory,1);
  const std::vector configs{Config(11),Config(12),Config(13,81)};
  ASSERT_TRUE(group.Load(configs,error)) << error;
  Publish(99);
  EXPECT_TRUE(counter.orders.empty());
  Publish(42);
  ASSERT_EQ(counter.orders.size(),2);
  Publish(81);
  trading.Run();
  ASSERT_EQ(counter.orders.size(),3);
  for (std::size_t i=0;i<3;++i) {
    EXPECT_EQ(counter.orders[i].strategy_id,11+i);
    EXPECT_EQ(counter.orders[i].order_id,(trading::OrderId{10,1+i}));
    EXPECT_EQ(counter.orders[i].price,91+i);
  }
  EXPECT_EQ(counter.orders[2].instrument_id,81);
  EXPECT_EQ(Reports(11),"11 10 1\n");
  EXPECT_EQ(Reports(12),"12 10 2\n");
  EXPECT_EQ(Reports(13),"13 10 3\n");
  ASSERT_TRUE(group.Stop());
  for (int id : {11,12,13}) EXPECT_TRUE(std::filesystem::exists(Marker(id)));
  ASSERT_TRUE(counter.sim.Fill({counter.orders[0].order_id,{"TEST","day","late"},91,1}));
  trading.Run();
  EXPECT_EQ(Reports(11),"11 10 1\n");
  Publish(42);
  EXPECT_EQ(counter.orders.size(),3);
  ASSERT_TRUE(group.Load(std::vector{Config(14)},error));
  Publish(42);
  EXPECT_EQ(counter.orders.back().order_id,(trading::OrderId{10,4}));
}

TEST_F(StrategyInstancesTest, OneInstanceDeclaresMultipleSubscriptionsAndCleansUpAll) {
  StrategyService group(10, market_factory, 1, trading_factory, 1);
  auto multi = Config(11);
  multi.parameters["instrument_id"] = "42 81";
  ASSERT_TRUE(group.Load(std::vector{multi, Config(12, 81)}, error)) << error;
  Publish(42);
  Publish(81);
  trading.Run();
  ASSERT_EQ(counter.orders.size(), 3);
  EXPECT_EQ(counter.orders[0].strategy_id, 11);
  EXPECT_EQ(counter.orders[0].instrument_id, 42);
  EXPECT_EQ(counter.orders[1].strategy_id, 11);
  EXPECT_EQ(counter.orders[1].instrument_id, 81);
  EXPECT_EQ(counter.orders[2].strategy_id, 12);
  ASSERT_TRUE(group.Stop());
  Publish(42);
  Publish(81);
  EXPECT_EQ(counter.orders.size(), 3);
}

TEST_F(StrategyInstancesTest, EmptySubscriptionListIsValidAndInvalidListsRollBack) {
  StrategyService group(10, market_factory, 1, trading_factory, 1);
  auto config = Config(11);
  config.parameters.erase("instrument_id");
  ASSERT_TRUE(group.Load(std::vector{config}, error)) << error;
  ASSERT_TRUE(group.Stop());
  for (const auto* invalid : {"0", "42 42"}) {
    config.parameters["instrument_id"] = invalid;
    EXPECT_FALSE(group.Load(std::vector{Config(12), config}, error));

    Publish(42);
    EXPECT_TRUE(counter.orders.empty());
  }
}

TEST_F(StrategyInstancesTest, GroupFailureRollsBackBeforeAnyUpstreamSubscription) {
  StrategyService group(10,market_factory,1,trading_factory,1);
  auto broken = Config(12);
  broken.library = FAIL_PLUGIN;
  EXPECT_FALSE(group.Load(std::vector{Config(11),broken,Config(13)},error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(std::filesystem::exists(Marker(11)));
  EXPECT_TRUE(std::filesystem::exists(Marker(12)));
  EXPECT_FALSE(std::filesystem::exists(Marker(13)));
  Publish(42);
  EXPECT_TRUE(counter.orders.empty());
  EXPECT_FALSE(group.Load(std::vector{Config(11),Config(11,81)},error));
  ASSERT_TRUE(group.Load(std::vector{Config(14)},error));
  EXPECT_FALSE(group.Load(std::vector{Config(15)},error));
  Publish(42);
  ASSERT_EQ(counter.orders.size(),1);
  EXPECT_EQ(counter.orders[0].strategy_id,14);
}

TEST_F(StrategyInstancesTest, StopInsideQuoteFailsWithoutInterruptingOtherInstances) {
  StrategyService group(10,market_factory,1,trading_factory,1);
  ASSERT_TRUE(group.Load(std::vector{Config(11),Config(12)},error));
  counter.on_submit = [&] { EXPECT_FALSE(group.Stop()); };
  Publish(42);
  EXPECT_EQ(counter.orders.size(),2);
  counter.on_submit = {};
  ASSERT_TRUE(group.Stop());  // Detaches queued accepts before the plugin libraries unload.
  trading.Run();
  EXPECT_EQ(Reports(11),"");
  EXPECT_EQ(Reports(12),"");
}

TEST_F(StrategyInstancesTest, LoadingDuringCallbacksFailsWithoutLeavingClientsOrPlugins) {
  StrategyService running(10, market_factory, 1, trading_factory, 1);
  ASSERT_TRUE(running.Load(std::vector{Config(11)}, error));
  counter.on_submit = [&] {
    StrategyService attempted(20, market_factory, 1, trading_factory, 1);
    EXPECT_FALSE(attempted.Load(std::vector{Config(12)}, error));
    EXPECT_TRUE(std::filesystem::exists(Marker(12)));
    EXPECT_TRUE(Reports(12).empty());
  };
  Publish(42);
  counter.on_submit = {};
  EXPECT_EQ(counter.orders.size(), 1);
  EXPECT_TRUE(running.Stop());
  StrategyService valid(20, market_factory, 1, trading_factory, 1);
  ASSERT_TRUE(valid.Load(std::vector{Config(12)}, error));
  Publish(42);
  EXPECT_EQ(counter.orders.size(), 2);
}

TEST_F(StrategyInstancesTest, TwoServicesCanReuseInstanceIdsWithDifferentCallers) {
  StrategyService a(10,market_factory,1,trading_factory,1), b(20,market_factory,1,trading_factory,1);
  ASSERT_TRUE(a.Load(std::vector{Config(11)},error));
  ASSERT_TRUE(b.Load(std::vector{Config(11)},error));
  Publish(42);
  ASSERT_EQ(counter.orders.size(),2);
  EXPECT_EQ(counter.orders[0].order_id,(trading::OrderId{10,1}));
  EXPECT_EQ(counter.orders[1].order_id,(trading::OrderId{20,1}));
  EXPECT_TRUE(a.Stop());
  trading.Run();
  EXPECT_EQ(Reports(11),"11 20 1\n");
}

}  // namespace
}  // namespace yquant::strategy
