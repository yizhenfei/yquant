#include "yquant/strategy/market_maker.h"

#include <limits>
#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::strategy {
namespace {
using namespace yquant::trading;

// Records what actually crossed the adapter boundary, including request order.
class Counter final : public ITradingAdapter {
 public:
  // ITradingAdapter implementation.
  void SetCallback(ITradingAdapterCallback& c) override { sim.SetCallback(c); }
  bool StartAccountSync(std::uint64_t id) override {
    return sim.StartAccountSync(id);
  }
  SendResult SubmitOrder(const SubmitOrderRequest& r) override {
    submits.push_back(r.order);
    return sim.SubmitOrder(r);
  }
  SendResult CancelOrder(const AdapterCancelOrderRequest& r) override {
    cancels.push_back(r);
    return sim.CancelOrder(r);
  }
  void Run() override { sim.Run(); }

  SimulatedTradingAdapter sim;
  std::vector<OrderInfo> submits;
  std::vector<AdapterCancelOrderRequest> cancels;
};

// Drives quotes and delayed counter events through the real local trading client.
class MakerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(service.BindAccount(7, counter));
    ASSERT_TRUE(registry.Register(3, service));
    client = TradingClientFactory(registry).Create(3);
    ASSERT_NE(client, nullptr);
    maker = std::make_unique<MarketMaker>(*client, ids, 7, 2, 42, 2);
    FundsTerms terms;
    terms.currency = model::Currency("CNY");
    terms.contract_multiplier = 1;
    terms.margin_rate = 0.1;
    terms.fees[0] = FeeRate{1, 0};
    for (auto side : {Side::kBuy, Side::kSell}) {
      ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, side, terms));
    }
    counter.sim.SetInitialState({terms.currency, 1000, 1000, 0, 0}, {});
    service.Run();
  }
  void Quote(double bid = 100, double ask = 102) {
    maker->OnQuote({42, bid, ask, 10, 10, ++seq});
  }
  void Start() {
    Quote();
    service.Run();
    ASSERT_EQ(counter.submits.size(), 2U);
  }
  Counter counter;
  TradingService service;
  TradingServiceRegistry registry;
  OrderIdGenerator ids{100};
  std::unique_ptr<TradingClient> client;
  std::unique_ptr<MarketMaker> maker;
  std::uint64_t seq = 0;
};

TEST_F(MakerTest, FixedBookHasOnlyTwoOrdersEvenBeforeAcknowledgement) {
  Quote();
  Quote();
  service.Run();
  Quote();
  ASSERT_EQ(counter.submits.size(), 2U);
  EXPECT_TRUE(counter.cancels.empty());
  EXPECT_EQ(counter.submits[0], (OrderInfo{{100, 1}, 7, 2, 42, Side::kBuy,
                                         Offset::kOpen, 100, 2}));
  EXPECT_EQ(counter.submits[1], (OrderInfo{{100, 2}, 7, 2, 42, Side::kSell,
                                         Offset::kOpen, 102, 2}));
}

TEST_F(MakerTest, BadBookAndForeignInstrumentProduceNoRequests) {
  maker->OnQuote({81, 100, 102, 1, 1, 1});
  maker->OnQuote({42, 100, 102, 0, 1, 2});
  Quote(103, 102);
  Quote(102, 102);
  Quote(0, 102);
  Quote(100, std::numeric_limits<double>::infinity());
  EXPECT_TRUE(counter.submits.empty());
}

TEST_F(MakerTest, DelayedAcceptanceAndCancellationUseLatestTarget) {
  counter.sim.SetNextSubmit({SendResult::kSent, std::nullopt});
  Start();
  Quote(99);
  Quote(98);
  EXPECT_TRUE(counter.cancels.empty());
  counter.sim.SetNextCancel({SendResult::kSent, std::nullopt});
  counter.sim.Emit({{100, 1}, AdapterOrderState::kAccepted});
  Quote(97);
  Quote(96);
  ASSERT_EQ(counter.cancels.size(), 1U);
  EXPECT_EQ(counter.submits.size(), 2U);
  counter.sim.EmitCancel({{100, 1}, 1, CancelResult::kCanceled});
  ASSERT_EQ(counter.submits.size(), 3U);
  EXPECT_EQ(counter.submits.back().price, 96);
  EXPECT_EQ(counter.submits.back().side, Side::kBuy);
}

TEST_F(MakerTest, CounterRejectionDoesNotLoopOnAnUnchangedPrice) {
  counter.sim.SetNextSubmit({SendResult::kSent, AdapterOrderState::kRejected});
  Start();
  Quote();
  Quote(100, 103);
  EXPECT_EQ(counter.submits.size(), 2U);
  ASSERT_EQ(counter.cancels.size(), 1U);
  EXPECT_EQ(counter.cancels[0].order_id, (OrderId{100, 2}));
  Quote(101, 103);
  ASSERT_EQ(counter.submits.size(), 3U);
  EXPECT_EQ(counter.submits.back().price, 101);
}

TEST_F(MakerTest, RejectedCancelLeavesLiveOrderAndRetriesOnNewTarget) {
  Start();
  counter.sim.SetNextCancel({SendResult::kSent, CancelResult::kRejected});
  Quote(99);
  service.Run();
  Quote(99);
  EXPECT_EQ(counter.submits.size(), 2U);
  EXPECT_EQ(counter.cancels.size(), 1U);
  Quote(98);
  service.Run();
  ASSERT_EQ(counter.cancels.size(), 2U);
  ASSERT_EQ(counter.submits.size(), 3U);
  EXPECT_EQ(counter.submits.back().price, 98);
}

TEST_F(MakerTest, FullFillDuringRepriceWaitsForCancelOutcome) {
  Start();
  ASSERT_TRUE(counter.sim.Fill({{100, 1}, {"s", "d", "1"}, 100, 1}));
  service.Run();
  EXPECT_EQ(counter.submits.size(), 2U);
  counter.sim.SetNextCancel({SendResult::kSent, std::nullopt});
  Quote(99);
  ASSERT_TRUE(counter.sim.Fill({{100, 1}, {"s", "d", "2"}, 100, 1}));
  service.Run();
  EXPECT_EQ(counter.submits.size(), 2U);
  Quote(98);
  counter.sim.EmitCancel({{100, 1}, 1, CancelResult::kRejected});
  ASSERT_EQ(counter.submits.size(), 3U);
  EXPECT_EQ(counter.submits.back().price, 98);
  EXPECT_EQ(counter.submits.back().quantity, 2);
}

TEST_F(MakerTest, FullCumulativeFillReplenishesAfterThePricedDetail) {
  Start();
  counter.sim.Emit({{100, 1}, AdapterOrderState::kAccepted, 2});
  Quote();
  EXPECT_EQ(counter.submits.size(), 2U);
  const TradeInfo trade{{100, 1}, {"s", "d", "fill"}, 100, 2};
  counter.sim.EmitTrade(trade);
  counter.sim.EmitTrade(trade);
  ASSERT_EQ(counter.submits.size(), 3U);
  EXPECT_EQ(counter.submits.back().price, 100);
}

TEST_F(MakerTest, ReplacementDoesNotCrossUncanceledOppositeOrder) {
  Start();
  counter.sim.SetNextCancel({SendResult::kSent, std::nullopt});
  Quote(105, 106);
  ASSERT_EQ(counter.cancels.size(), 2U);
  counter.sim.EmitCancel({{100, 1}, 1, CancelResult::kCanceled});
  EXPECT_EQ(counter.submits.size(), 2U);
  counter.sim.EmitCancel({{100, 2}, 2, CancelResult::kCanceled});
  ASSERT_EQ(counter.submits.size(), 4U);
  EXPECT_EQ(counter.submits[2].price, 105);
  EXPECT_EQ(counter.submits[3].price, 106);
}

TEST_F(MakerTest, SendFailureKeepsIdentityForLateAcceptance) {
  counter.sim.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  Start();
  Quote(99);
  EXPECT_EQ(counter.submits.size(), 2U);
  counter.sim.Emit({{100, 1}, AdapterOrderState::kAccepted});
  ASSERT_EQ(counter.cancels.size(), 1U);
  EXPECT_EQ(counter.cancels.back().order_id, (OrderId{100, 1}));
}
TEST_F(MakerTest, InsufficientFundsWaitsForANewTargetBeforeRetry) {
  FundsTerms terms;
  terms.currency = model::Currency("CNY");
  terms.contract_multiplier = 1;
  terms.margin_rate = 100;
  terms.fees[0] = FeeRate{};
  ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kBuy, terms));
  Quote();
  service.Run();
  Quote();
  service.Run();
  ASSERT_EQ(counter.submits.size(), 1U);
  EXPECT_EQ(counter.submits[0].side, Side::kSell);
  Quote(1);
  ASSERT_EQ(counter.submits.size(), 2U);
  EXPECT_EQ(counter.submits.back().order_id, (OrderId{100, 3}));
}

TEST_F(MakerTest, FailedCancelDoesNotSendReplacementOrRepeatTheAttempt) {
  Start();
  counter.sim.SetNextCancel({SendResult::kNotSent, std::nullopt});
  Quote(99);
  service.Run();
  Quote(99);
  EXPECT_EQ(counter.submits.size(), 2U);
  EXPECT_EQ(counter.cancels.size(), 1U);
  Quote(98);
  service.Run();
  ASSERT_EQ(counter.submits.size(), 3U);
  EXPECT_EQ(counter.submits.back().price, 98);
}

TEST_F(MakerTest, LatePricedFillOnCanceledOrderLeavesReplacementIntact) {
  Start();
  Quote(99);
  service.Run();
  ASSERT_EQ(counter.submits.size(), 3U);
  counter.sim.EmitTrade({{100, 1}, {"s", "d", "late"}, 100, 1});
  Quote(99);
  EXPECT_EQ(counter.submits.size(), 3U);
  EXPECT_EQ(counter.cancels.size(), 1U);
}

TEST_F(MakerTest, ExhaustedSharedGeneratorDoesNotReuseAnOrderId) {
  OrderIdGenerator limited(101, std::numeric_limits<std::uint64_t>::max() - 1);
  MarketMaker last(*client, limited, 7, 3, 42, 1);
  last.OnQuote({42, 100, 102, 1, 1, 1});
  service.Run();
  last.OnQuote({42, 100, 103, 1, 1, 2});
  ASSERT_EQ(counter.submits.size(), 1U);
  EXPECT_EQ(counter.submits[0].order_id.sequence,
            std::numeric_limits<std::uint64_t>::max());
}
}  // namespace
}  // namespace yquant::strategy
