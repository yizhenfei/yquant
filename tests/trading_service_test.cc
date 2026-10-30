#include "yquant/trading/trading_service.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/trading/internal/call_context.h"
#include "yquant/trading/order.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_listener.h"

namespace yquant::trading {
namespace {

// Keeps callback values and checks the public callback timing and thread rules.
class Listener final : public TradingListener {
 public:
  // TradingListener implementation.
  void OnRequestFailure(const RequestFailure& failure) override {
    EXPECT_FALSE(in_request);
    EXPECT_EQ(thread, std::this_thread::get_id());
    if (failure.snapshot) {
      EXPECT_EQ(failure.order_id, failure.snapshot->order.order_id);
    }
    failures.push_back(failure);
    sequence.push_back("failure");
  }
  void OnOrderReport(const OrderReport& report) override {
    EXPECT_FALSE(in_request);
    EXPECT_EQ(thread, std::this_thread::get_id());
    reports.push_back(report);
    sequence.push_back("order");
    if (action) {
      action(report);
    }
  }
  void OnCancelReport(const CancelReport&) override {
    ADD_FAILURE() << "Unexpected cancellation report";
  }

  bool in_request = false;
  std::thread::id thread = std::this_thread::get_id();
  std::vector<RequestFailure> failures;
  std::vector<OrderReport> reports;
  std::vector<std::string> sequence;
  std::function<void(const OrderReport&)> action;
};

// Supplies two independently controlled accounts and the normal order input.
class TradingServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(service.BindAccount(7, first));
    ASSERT_TRUE(service.BindAccount(8, second));
  }
  RequestResult Submit(SubmitOrderRequest input, Listener& listener,
                       internal::CallContext call = {10, 1}) {
    listener.in_request = true;
    const auto result = service.SubmitOrder(input, listener, call);
    listener.in_request = false;
    return result;
  }
  SimulatedTradingAdapter first;
  SimulatedTradingAdapter second;
  Listener original;
  Listener other;
  TradingService service;
  SubmitOrderRequest request{{{100, 1}, 7, 2, 42, Side::kBuy,
                             Offset::kOpen, 100.5, 3}};
};

TEST_F(TradingServiceTest, RoutesAccountsAndKeepsOriginalParameters) {
  const auto saved = request.order;
  EXPECT_EQ(Submit(request, original), RequestResult::kDelivered);
  request.order.account_id = 8;
  request.order.strategy_id = 9;
  request.order.order_id = {200, 1};
  second.SetNextSubmit(
      {SendResult::kSent, AdapterOrderState::kRejected});
  EXPECT_EQ(Submit(request, other, {11, 1}), RequestResult::kDelivered);
  EXPECT_TRUE(original.reports.empty());
  request.order.price = 1;
  service.Run();
  ASSERT_EQ(original.reports.size(), 1U);
  ASSERT_EQ(other.reports.size(), 1U);
  EXPECT_EQ(original.reports[0].snapshot.order, saved);
  EXPECT_EQ(original.reports[0].snapshot.state, OrderState::kAccepted);
  EXPECT_EQ(other.reports[0].snapshot.state, OrderState::kRejected);
  EXPECT_EQ(other.reports[0].snapshot.order.account_id, 8U);
  EXPECT_EQ(other.reports[0].snapshot.order.strategy_id, 9U);
  EXPECT_EQ(other.reports[0].snapshot.order.order_id, (OrderId{200, 1}));
  EXPECT_EQ(first.FindOrder({200, 1}), nullptr);
  EXPECT_EQ(second.FindOrder({100, 1}), nullptr);
}

TEST_F(TradingServiceTest, DuplicateBindingsDoNotReplaceOriginalAccount) {
  SimulatedTradingAdapter unused;
  EXPECT_FALSE(service.BindAccount(7, unused));
  EXPECT_FALSE(service.BindAccount(9, first));
  Submit(request, original);
  service.Run();
  EXPECT_NE(first.FindOrder(request.order.order_id), nullptr);
  EXPECT_EQ(unused.FindOrder(request.order.order_id), nullptr);
}

TEST_F(TradingServiceTest, DuplicateIdFailureBelongsToNewInvocationOnly) {
  Submit(request, original, {10, 1});
  auto duplicate = request;
  duplicate.order.account_id = 8;
  duplicate.order.strategy_id = 99;
  Submit(duplicate, other, {11, 5});
  EXPECT_TRUE(other.failures.empty());
  service.Run();
  ASSERT_EQ(other.failures.size(), 1U);
  EXPECT_EQ(other.failures[0].reason, FailureReason::kDuplicateOrder);
  EXPECT_EQ(other.failures[0].order_id, duplicate.order.order_id);
  EXPECT_FALSE(other.failures[0].snapshot);
  ASSERT_EQ(original.reports.size(), 1U);
  EXPECT_EQ(original.reports[0].snapshot.order, request.order);
  EXPECT_TRUE(original.failures.empty());
  EXPECT_EQ(second.FindOrder(request.order.order_id), nullptr);
}

TEST_F(TradingServiceTest, UnknownAccountFailsWithoutCreatingOrder) {
  auto invalid = request;
  invalid.order.account_id = 9;
  Submit(invalid, other);
  service.Run();
  ASSERT_EQ(other.failures.size(), 1U);
  EXPECT_EQ(other.failures[0].reason, FailureReason::kUnknownAccount);
  EXPECT_EQ(other.failures[0].order_id, invalid.order.order_id);
  EXPECT_FALSE(other.failures[0].snapshot);
  Submit(request, original);
  service.Run();
  EXPECT_EQ(original.reports.size(), 1U);
}

TEST_F(TradingServiceTest, InvalidFieldsFailBeforeAdapterCall) {
  const auto check = [&](SubmitOrderRequest invalid) {
    other.failures.clear();
    Submit(invalid, other);
    service.Run();
    ASSERT_EQ(other.failures.size(), 1U);
    EXPECT_EQ(other.failures[0].reason, FailureReason::kInvalidRequest);
    EXPECT_EQ(other.failures[0].order_id, invalid.order.order_id);
    EXPECT_FALSE(other.failures[0].snapshot);
    EXPECT_EQ(first.FindOrder(invalid.order.order_id), nullptr);
  };
  auto invalid = request;
  invalid.order.price = std::numeric_limits<double>::quiet_NaN();
  check(invalid);
  invalid.order.price = std::numeric_limits<double>::infinity();
  check(invalid);
  invalid = request;
  invalid.order.price = 0;
  check(invalid);
  invalid = request;
  invalid.order.quantity = -1;
  check(invalid);
  invalid = request;
  invalid.order.instrument_id = 0;
  check(invalid);
  invalid = request;
  invalid.order.side = static_cast<Side>(99);
  check(invalid);
  invalid = request;
  invalid.order.offset = static_cast<Offset>(99);
  check(invalid);
  invalid = request;
  invalid.order.order_id.sequence = 0;
  check(invalid);
  Submit(request, original);
  service.Run();
  EXPECT_EQ(original.reports.size(), 1U);
}

TEST_F(TradingServiceTest, NotSentProducesFailureWithRetainedSnapshot) {
  first.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  EXPECT_EQ(Submit(request, original), RequestResult::kDelivered);
  EXPECT_TRUE(original.failures.empty());
  service.Run();
  ASSERT_EQ(original.failures.size(), 1U);
  const auto& failure = original.failures[0];
  EXPECT_EQ(failure.order_id, request.order.order_id);
  EXPECT_EQ(failure.reason, FailureReason::kSendFailed);
  EXPECT_FALSE(failure.cancel_request_id);
  ASSERT_TRUE(failure.snapshot);
  EXPECT_EQ(failure.snapshot->state, OrderState::kSendFailed);
  EXPECT_EQ(failure.snapshot->order, request.order);
  EXPECT_TRUE(original.reports.empty());
  Submit(request, other);
  service.Run();
  ASSERT_EQ(other.failures.size(), 1U);
  EXPECT_EQ(other.failures[0].reason, FailureReason::kDuplicateOrder);
}

TEST_F(TradingServiceTest, SentOrderWaitsForCounterReport) {
  first.SetNextSubmit({SendResult::kSent, std::nullopt});
  Submit(request, original);
  service.Run();
  service.Run();
  EXPECT_TRUE(original.reports.empty());
  EXPECT_TRUE(original.failures.empty());
  first.Emit({request.order.order_id, AdapterOrderState::kAccepted});
  ASSERT_EQ(original.reports.size(), 1U);
  EXPECT_EQ(original.reports[0].snapshot.state, OrderState::kAccepted);
  EXPECT_EQ(original.sequence, (std::vector<std::string>{"order"}));
  service.Run();
  EXPECT_EQ(original.reports.size(), 1U);
}

TEST_F(TradingServiceTest, LateValidReportCorrectsEarlierTransmissionFailure) {
  first.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  Submit(request, original);
  first.Emit({request.order.order_id, AdapterOrderState::kAccepted});
  EXPECT_TRUE(original.reports.empty());
  service.Run();
  ASSERT_EQ(original.failures.size(), 1U);
  ASSERT_EQ(original.reports.size(), 1U);
  ASSERT_TRUE(original.failures[0].snapshot);
  EXPECT_EQ(original.failures[0].snapshot->state, OrderState::kSendFailed);
  EXPECT_EQ(original.reports[0].snapshot.state, OrderState::kAccepted);
  EXPECT_LT(original.failures[0].snapshot->version,
            original.reports[0].snapshot.version);
  EXPECT_EQ(original.sequence, (std::vector<std::string>{"failure", "order"}));
}

TEST_F(TradingServiceTest, InvalidAndContradictoryReportsDoNotPolluteOrders) {
  first.SetNextSubmit({SendResult::kSent, std::nullopt});
  Submit(request, original);
  first.Emit({{100, 999}, AdapterOrderState::kAccepted});
  second.Emit({request.order.order_id, AdapterOrderState::kAccepted});
  first.Emit({request.order.order_id, static_cast<AdapterOrderState>(99)});
  EXPECT_TRUE(original.reports.empty());
  first.Emit({request.order.order_id, AdapterOrderState::kAccepted});
  ASSERT_EQ(original.reports.size(), 1U);
  const auto version = original.reports[0].snapshot.version;
  first.Emit({request.order.order_id, AdapterOrderState::kAccepted});
  first.Emit({request.order.order_id, AdapterOrderState::kRejected});
  EXPECT_EQ(original.reports.size(), 1U);
  EXPECT_EQ(original.reports[0].snapshot.version, version);
}

TEST_F(TradingServiceTest, CallbackCanSubmitWhileCurrentSnapshotStaysValid) {
  first.SetNextSubmit(
      {SendResult::kSent, AdapterOrderState::kAccepted});
  Submit(request, original);
  original.action = [&](const OrderReport& report) {
    const auto saved = report;
    service.Run();
    EXPECT_FALSE(service.BindAccount(9, first));
    // Exceeds the reserved notification capacity while the report is borrowed.
    for (std::uint64_t seq = 2; seq < 302; ++seq) {
      auto next = request;
      next.order.order_id.sequence = seq;
      next.order.account_id = 8;
      second.SetNextSubmit(
          {SendResult::kSent, AdapterOrderState::kAccepted});
      Submit(next, other);
    }
    EXPECT_TRUE(other.reports.empty());
    EXPECT_EQ(report.snapshot.order, saved.snapshot.order);
    EXPECT_EQ(report.snapshot.version, saved.snapshot.version);
  };
  service.Run();
  EXPECT_EQ(original.reports.size(), 1U);
  EXPECT_EQ(other.reports.size(), 64U);
  while (other.reports.size() < 300) {
    service.Run();
  }
  EXPECT_EQ(other.reports.size(), 300U);
  for (std::size_t index = 0; index < other.reports.size(); ++index) {
    EXPECT_EQ(other.reports[index].snapshot.order.order_id,
              (OrderId{100, index + 2}));
  }
}

TEST_F(TradingServiceTest, BusyNotificationQueueDoesNotStarveOtherAccount) {
  for (std::uint64_t seq = 1; seq <= 70; ++seq) {
    request.order.order_id.sequence = seq;
    first.SetNextSubmit(
        {SendResult::kNotSent, std::nullopt});
    Submit(request, original);
  }
  request.order.order_id = {200, 1};
  request.order.account_id = 8;
  Submit(request, other);
  service.Run();
  EXPECT_EQ(original.failures.size(), 64U);
  EXPECT_EQ(other.reports.size(), 1U);
  service.Run();
  EXPECT_EQ(original.failures.size(), 70U);
}

// Calls Run from inside SubmitOrder to check the request-side reentry guard.
class ReentrantAdapter final : public ITradingAdapter {
 public:
  explicit ReentrantAdapter(TradingService& service) : service(service) {}

  // ITradingAdapter implementation.
  void SetCallback(ITradingAdapterCallback& value) override {
    callback = &value;
  }
  SendResult SubmitOrder(const SubmitOrderRequest& request) override {
    service.Run();
    pending_order = request.order.order_id;
    service.Run();
    return SendResult::kSent;
  }
  SendResult CancelOrder(const AdapterCancelOrderRequest&) override {
    ADD_FAILURE() << "Unexpected cancellation request";
    return SendResult::kNotSent;
  }
  void Run() override {
    ++runs;
    if (pending_order) {
      const auto id = *pending_order;
      pending_order.reset();
      callback->OnOrderReport({id, AdapterOrderState::kAccepted});
    }
  }

  TradingService& service;
  ITradingAdapterCallback* callback = nullptr;
  int runs = 0;
  std::optional<OrderId> pending_order;
};

TEST(TradingServiceReentryTest, RunDuringSubmissionReturnsImmediately) {
  TradingService service;
  ReentrantAdapter adapter(service);
  Listener listener;
  ASSERT_TRUE(service.BindAccount(7, adapter));
  listener.in_request = true;
  EXPECT_EQ(service.SubmitOrder({{{100, 1}, 7, 2, 42, Side::kBuy,
                                  Offset::kOpen, 100, 1}}, listener, {10, 1}),
            RequestResult::kDelivered);
  listener.in_request = false;
  EXPECT_EQ(adapter.runs, 0);
  EXPECT_TRUE(listener.reports.empty());
  service.Run();
  EXPECT_EQ(adapter.runs, 1);
  EXPECT_EQ(listener.reports.size(), 1U);
}

}  // namespace
}  // namespace yquant::trading
