#include "yquant/trading/simulated_trading_adapter.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/trading/order.h"
#include "yquant/trading/order_id_generator.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_service.h"

namespace yquant::trading {
namespace {

// Retains independent reports and optionally exercises nested callback actions.
class AdapterListener final : public ITradingAdapterCallback {
 public:
  // ITradingAdapterCallback implementation.
  void OnOrderReport(const AdapterOrderReport& report) override {
    EXPECT_EQ(std::this_thread::get_id(), thread);
    reports.push_back(report);
    if (action) {
      action(report);
    }
  }

  std::thread::id thread = std::this_thread::get_id();
  std::vector<AdapterOrderReport> reports;
  std::function<void(const AdapterOrderReport&)> action;
};

TEST(OrderIdGeneratorTest, SharesSequenceAcrossAllCallerUses) {
  OrderIdGenerator caller(100);
  OrderIdGenerator other(200);
  EXPECT_EQ(caller.Next(), (OrderId{100, 1}));
  EXPECT_EQ(other.Next(), (OrderId{200, 1}));
  EXPECT_EQ(caller.Next(), (OrderId{100, 2}));
  EXPECT_EQ(caller.Next(), (OrderId{100, 3}));
}

TEST(OrderIdGeneratorTest, ExhaustionDoesNotWrapOrReuseIds) {
  constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
  OrderIdGenerator caller(100, kMax - 1);
  EXPECT_EQ(caller.Next(), (OrderId{100, kMax}));
  EXPECT_FALSE(caller.Next());
  EXPECT_FALSE(caller.Next());
}

TEST(SimulatedTradingAdapterTest, SavesInputAndDeliversDelayedReport) {
  SimulatedTradingAdapter adapter;
  AdapterListener listener;
  adapter.SetCallback(listener);
  SubmitOrderRequest request{{{100, 1}, 7, 2, 42, Side::kSell,
                             Offset::kCloseToday, 100.5, 3}};
  const auto original = request.order;
  EXPECT_EQ(adapter.SubmitOrder(request), SendResult::kSent);
  EXPECT_TRUE(listener.reports.empty());
  request.order.price = 1;
  request.order.order_id.sequence = 99;
  ASSERT_NE(adapter.FindOrder(original.order_id), nullptr);
  EXPECT_EQ(*adapter.FindOrder(original.order_id), original);
  adapter.Run();
  ASSERT_EQ(listener.reports.size(), 1U);
  EXPECT_EQ(listener.reports[0].order_id, original.order_id);
  EXPECT_EQ(listener.reports[0].state, AdapterOrderState::kAccepted);
  adapter.Run();
  EXPECT_EQ(listener.reports.size(), 1U);
}

TEST(SimulatedTradingAdapterTest, ControlsSendResultSeparatelyFromReports) {
  SimulatedTradingAdapter adapter;
  AdapterListener listener;
  adapter.SetCallback(listener);
  SubmitOrderRequest request{{{100, 1}, 7, 2, 42, Side::kBuy,
                             Offset::kOpen, 100, 1}};
  adapter.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  EXPECT_EQ(adapter.SubmitOrder(request), SendResult::kNotSent);
  EXPECT_EQ(adapter.FindOrder(request.order.order_id), nullptr);
  adapter.Run();
  EXPECT_TRUE(listener.reports.empty());

  request.order.order_id.sequence = 2;
  adapter.SetNextSubmit({SendResult::kSent, std::nullopt});
  EXPECT_EQ(adapter.SubmitOrder(request), SendResult::kSent);
  ASSERT_NE(adapter.FindOrder(request.order.order_id), nullptr);
  EXPECT_TRUE(listener.reports.empty());
  adapter.Emit({request.order.order_id, AdapterOrderState::kAccepted});
  ASSERT_EQ(listener.reports.size(), 1U);

  request.order.order_id.sequence = 3;
  adapter.SetNextSubmit(
      {SendResult::kNotSent, AdapterOrderState::kAccepted});
  EXPECT_EQ(adapter.SubmitOrder(request), SendResult::kNotSent);
  EXPECT_EQ(listener.reports.size(), 1U);
  adapter.Run();
  ASSERT_EQ(listener.reports.size(), 2U);
  EXPECT_EQ(listener.reports[1].order_id, request.order.order_id);

  request.order.order_id.sequence = 4;
  adapter.SetNextSubmit(
      {SendResult::kSent, AdapterOrderState::kRejected});
  EXPECT_EQ(adapter.SubmitOrder(request), SendResult::kSent);
  EXPECT_EQ(listener.reports.size(), 2U);
  adapter.Run();
  ASSERT_EQ(listener.reports.size(), 3U);
  EXPECT_EQ(listener.reports[2].state, AdapterOrderState::kRejected);
}

TEST(SimulatedTradingAdapterTest, RejectsDuplicateIdsWithoutReplacingInput) {
  SimulatedTradingAdapter adapter;
  AdapterListener listener;
  adapter.SetCallback(listener);
  SubmitOrderRequest request{{{100, 1}, 7, 2, 42, Side::kBuy,
                             Offset::kOpen, 100, 1}};
  EXPECT_EQ(adapter.SubmitOrder(request), SendResult::kSent);
  request.order.price = 200;
  EXPECT_EQ(adapter.SubmitOrder(request), SendResult::kNotSent);
  ASSERT_NE(adapter.FindOrder(request.order.order_id), nullptr);
  EXPECT_EQ(adapter.FindOrder(request.order.order_id)->price, 100);
  adapter.Run();
  EXPECT_EQ(listener.reports.size(), 1U);
}

TEST(SimulatedTradingAdapterTest, BatchesWorkAndPreventsNestedRun) {
  SimulatedTradingAdapter adapter;
  AdapterListener listener;
  adapter.SetCallback(listener);
  for (std::uint64_t seq = 1; seq <= 65; ++seq) {
    EXPECT_EQ(adapter.SubmitOrder({{{100, seq}, 7, 2, 42, Side::kBuy,
                                     Offset::kOpen, 100, 1}}),
              SendResult::kSent);
  }
  listener.action = [&](const AdapterOrderReport&) { adapter.Run(); };
  adapter.Run();
  EXPECT_EQ(listener.reports.size(), 64U);
  adapter.Run();
  EXPECT_EQ(listener.reports.size(), 65U);
  EXPECT_EQ(listener.reports.front().order_id, (OrderId{100, 1}));
}

TEST(SimulatedTradingAdapterTest, SubmissionWithoutCallbackIsNotSent) {
  SimulatedTradingAdapter adapter;
  EXPECT_EQ(adapter.SubmitOrder({}), SendResult::kNotSent);
}

}  // namespace
}  // namespace yquant::trading
