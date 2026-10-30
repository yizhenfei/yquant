#include "yquant/trading/trading_client_factory.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/trading/order.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_listener.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {
namespace {

// Retains business results and permits callback actions to exercise reentry.
class Results final : public TradingListener {
 public:
  // TradingListener implementation.
  void OnRequestFailure(const RequestFailure& failure) override {
    EXPECT_FALSE(in_request);
    EXPECT_EQ(thread, std::this_thread::get_id());
    failures.push_back(failure);
  }
  void OnOrderReport(const OrderReport& report) override {
    EXPECT_FALSE(in_request);
    EXPECT_EQ(thread, std::this_thread::get_id());
    orders.push_back(report);
    if (on_order) {
      on_order(report);
    }
  }
  void OnCancelReport(const CancelReport& report) override {
    EXPECT_FALSE(in_request);
    EXPECT_EQ(thread, std::this_thread::get_id());
    cancels.push_back(report);
    if (on_cancel) {
      on_cancel(report);
    }
  }

  bool in_request = false;
  std::thread::id thread = std::this_thread::get_id();
  std::vector<RequestFailure> failures;
  std::vector<OrderReport> orders;
  std::vector<CancelReport> cancels;
  std::function<void(const OrderReport&)> on_order;
  std::function<void(const CancelReport&)> on_cancel;
};

// Records adapter arguments while using the actual simulated order book.
class RecordedAdapter final : public ITradingAdapter {
 public:
  RecordedAdapter() = default;
  RecordedAdapter(const RecordedAdapter&) = delete;
  RecordedAdapter& operator=(const RecordedAdapter&) = delete;

  // ITradingAdapter implementation.
  void SetCallback(ITradingAdapterCallback& callback) override {
    counter.SetCallback(callback);
  }
  SendResult SubmitOrder(const SubmitOrderRequest& request) override {
    return counter.SubmitOrder(request);
  }
  SendResult CancelOrder(const AdapterCancelOrderRequest& request) override {
    requests.push_back(request);
    return counter.CancelOrder(request);
  }
  void Run() override { counter.Run(); }

  SimulatedTradingAdapter counter;
  std::vector<AdapterCancelOrderRequest> requests;
};

// Assembles the public client path with two independent account adapters.
class CancellationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(service.BindAccount(7, first));
    ASSERT_TRUE(service.BindAccount(8, second));
    ASSERT_TRUE(registry.Register(3, service));
    client = TradingClientFactory(registry).Create(3);
    ASSERT_NE(client, nullptr);
  }
  void Open() {
    ASSERT_EQ(client->SubmitOrder(submit, owner), RequestResult::kDelivered);
    service.Run();
    ASSERT_EQ(owner.orders.size(), 1U);
  }
  CancelOrderRequest Request(std::uint64_t id) const {
    return {submit.order.order_id, 7, 2, id};
  }
  void Cancel(const CancelOrderRequest& request, Results& listener) {
    listener.in_request = true;
    EXPECT_EQ(client->CancelOrder(request, listener),
              RequestResult::kDelivered);
    listener.in_request = false;
  }
  RecordedAdapter first;
  RecordedAdapter second;
  Results owner;
  Results attempt;
  Results other;
  Results failures;
  TradingService service;
  TradingServiceRegistry registry;
  std::unique_ptr<TradingClient> client;
  SubmitOrderRequest submit{{{100, 1}, 7, 2, 42, Side::kBuy,
                             Offset::kOpen, 100.5, 5}};
};

TEST_F(CancellationTest, SuccessNotifiesOwnerBeforeCancellationListener) {
  Open();
  std::vector<std::string> sequence;
  owner.on_order = [&](const OrderReport&) { sequence.push_back("order"); };
  attempt.on_cancel = [&](const CancelReport&) {
    sequence.push_back("cancel");
  };
  auto request = Request(41);
  Cancel(request, attempt);
  request.cancel_request_id = 999;
  EXPECT_TRUE(attempt.cancels.empty());
  ASSERT_EQ(first.requests.size(), 1U);
  EXPECT_EQ(first.requests[0].order_id, submit.order.order_id);
  EXPECT_NE(first.requests[0].service_cancel_id, 0U);
  EXPECT_TRUE(second.requests.empty());
  service.Run();
  ASSERT_EQ(attempt.cancels.size(), 1U);
  EXPECT_EQ(attempt.cancels[0].cancel_request_id, 41U);
  EXPECT_EQ(attempt.cancels[0].result, CancelResult::kCanceled);
  EXPECT_EQ(attempt.cancels[0].snapshot.order, submit.order);
  EXPECT_EQ(attempt.cancels[0].snapshot.state, OrderState::kCanceled);
  ASSERT_EQ(owner.orders.size(), 2U);
  EXPECT_EQ(owner.orders.back().snapshot.state, OrderState::kCanceled);
  EXPECT_LT(owner.orders.front().snapshot.version,
            owner.orders.back().snapshot.version);
  EXPECT_EQ(sequence, (std::vector<std::string>{"order", "cancel"}));
  EXPECT_TRUE(attempt.orders.empty());
  EXPECT_TRUE(attempt.failures.empty());
  EXPECT_TRUE(owner.cancels.empty());
}

TEST_F(CancellationTest, CounterRejectionKeepsOrderStateAndVersion) {
  Open();
  first.counter.SetNextCancel({SendResult::kSent, CancelResult::kRejected});
  Cancel(Request(1), attempt);
  service.Run();
  ASSERT_EQ(attempt.cancels.size(), 1U);
  EXPECT_EQ(attempt.cancels[0].result, CancelResult::kRejected);
  EXPECT_EQ(attempt.cancels[0].snapshot.state, OrderState::kAccepted);
  EXPECT_EQ(attempt.cancels[0].snapshot.version,
            owner.orders[0].snapshot.version);
  EXPECT_EQ(owner.orders.size(), 1U);
  EXPECT_TRUE(attempt.failures.empty());
}

TEST_F(CancellationTest, SendFailureConsumesAttemptIdAndAllowsNewAttempt) {
  Open();
  first.counter.SetNextCancel({SendResult::kNotSent, std::nullopt});
  Cancel(Request(5), attempt);
  EXPECT_TRUE(attempt.failures.empty());
  service.Run();
  ASSERT_EQ(attempt.failures.size(), 1U);
  EXPECT_EQ(attempt.failures[0].reason, FailureReason::kSendFailed);
  EXPECT_EQ(attempt.failures[0].cancel_request_id, 5U);
  ASSERT_TRUE(attempt.failures[0].snapshot);
  EXPECT_EQ(attempt.failures[0].snapshot->state, OrderState::kAccepted);
  EXPECT_EQ(owner.orders.size(), 1U);
  Cancel(Request(5), other);
  service.Run();
  ASSERT_EQ(other.failures.size(), 1U);
  EXPECT_EQ(other.failures[0].reason, FailureReason::kDuplicateCancel);
  EXPECT_FALSE(other.failures[0].snapshot);
  Cancel(Request(6), other);
  service.Run();
  ASSERT_EQ(other.cancels.size(), 1U);
  EXPECT_EQ(other.cancels[0].cancel_request_id, 6U);
  EXPECT_EQ(first.requests.size(), 2U);
}

TEST_F(CancellationTest, ValidationFailuresLeaveOriginalOrderUntouched) {
  Open();
  const auto check = [&](CancelOrderRequest request, FailureReason reason) {
    failures.failures.clear();
    Cancel(request, failures);
    service.Run();
    ASSERT_EQ(failures.failures.size(), 1U);
    EXPECT_EQ(failures.failures[0].reason, reason);
    EXPECT_EQ(failures.failures[0].order_id, request.order_id);
    ASSERT_TRUE(failures.failures[0].cancel_request_id);
    EXPECT_EQ(*failures.failures[0].cancel_request_id,
              request.cancel_request_id);
    EXPECT_FALSE(failures.failures[0].snapshot);
    EXPECT_TRUE(first.requests.empty());
    EXPECT_TRUE(second.requests.empty());
    EXPECT_EQ(owner.orders.size(), 1U);
  };
  auto request = Request(1);
  request.order_id.sequence = 999;
  check(request, FailureReason::kUnknownOrder);
  request = Request(1);
  request.account_id = 8;
  check(request, FailureReason::kOwnershipMismatch);
  request = Request(1);
  request.strategy_id = 9;
  check(request, FailureReason::kOwnershipMismatch);
  check(Request(0), FailureReason::kInvalidRequest);
  Cancel(Request(1), attempt);
  service.Run();
  EXPECT_EQ(attempt.cancels.size(), 1U);
}

TEST_F(CancellationTest, InactiveOrdersCannotBeCanceled) {
  first.counter.SetNextSubmit(
      {SendResult::kSent, AdapterOrderState::kRejected});
  Open();
  Cancel(Request(1), attempt);
  service.Run();
  ASSERT_EQ(attempt.failures.size(), 1U);
  EXPECT_EQ(attempt.failures[0].reason, FailureReason::kOrderInactive);
  EXPECT_TRUE(first.requests.empty());
  auto failed = submit;
  failed.order.order_id.sequence = 2;
  first.counter.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  EXPECT_EQ(client->SubmitOrder(failed, owner), RequestResult::kDelivered);
  service.Run();
  auto request = Request(2);
  request.order_id = failed.order.order_id;
  Cancel(request, other);
  service.Run();
  ASSERT_EQ(other.failures.size(), 1U);
  EXPECT_EQ(other.failures[0].reason, FailureReason::kOrderInactive);
  EXPECT_TRUE(first.requests.empty());
}

TEST_F(CancellationTest, CallerIdsCanRepeatAcrossAccountsAndOrders) {
  Open();
  auto another = submit;
  another.order.order_id.sequence = 2;
  another.order.account_id = 8;
  EXPECT_EQ(client->SubmitOrder(another, owner), RequestResult::kDelivered);
  service.Run();
  Cancel(Request(1), attempt);
  Cancel({another.order.order_id, 8, 2, 1}, other);
  service.Run();
  ASSERT_EQ(attempt.cancels.size(), 1U);
  ASSERT_EQ(other.cancels.size(), 1U);
  EXPECT_EQ(attempt.cancels[0].order_id, submit.order.order_id);
  EXPECT_EQ(other.cancels[0].order_id, another.order.order_id);
  ASSERT_EQ(first.requests.size(), 1U);
  ASSERT_EQ(second.requests.size(), 1U);
  EXPECT_NE(first.requests[0].service_cancel_id,
            second.requests[0].service_cancel_id);
}

TEST_F(CancellationTest, LateAttemptCannotCompleteOrUndoAnotherAttempt) {
  Open();
  first.counter.SetNextCancel({SendResult::kSent, std::nullopt});
  Cancel(Request(1), attempt);
  Cancel(Request(2), other);
  service.Run();
  EXPECT_TRUE(attempt.cancels.empty());
  ASSERT_EQ(other.cancels.size(), 1U);
  ASSERT_EQ(owner.orders.size(), 2U);
  const auto version = owner.orders.back().snapshot.version;
  const auto old = first.requests[0];
  const auto newer = first.requests[1];
  first.counter.EmitCancel({old.order_id, old.service_cancel_id,
                            CancelResult::kRejected});
  ASSERT_EQ(attempt.cancels.size(), 1U);
  EXPECT_EQ(attempt.cancels[0].cancel_request_id, 1U);
  EXPECT_EQ(attempt.cancels[0].snapshot.state, OrderState::kCanceled);
  first.counter.EmitCancel({old.order_id, old.service_cancel_id,
                            CancelResult::kRejected});
  first.counter.EmitCancel({old.order_id, old.service_cancel_id,
                            CancelResult::kCanceled});
  first.counter.EmitCancel({newer.order_id, newer.service_cancel_id,
                            CancelResult::kCanceled});
  first.counter.EmitCancel({newer.order_id, newer.service_cancel_id,
                            CancelResult::kRejected});
  EXPECT_EQ(attempt.cancels.size(), 1U);
  EXPECT_EQ(other.cancels.size(), 1U);
  EXPECT_EQ(owner.orders.size(), 2U);
  EXPECT_EQ(owner.orders.back().snapshot.version, version);
}

TEST_F(CancellationTest, InvalidEventsCannotChangeOrCompleteAnAttempt) {
  Open();
  first.counter.SetNextCancel({SendResult::kSent, std::nullopt});
  Cancel(Request(11), attempt);
  const auto sent = first.requests[0];
  first.counter.EmitCancel({sent.order_id, sent.service_cancel_id + 1,
                            CancelResult::kCanceled});
  first.counter.EmitCancel({{100, 999}, sent.service_cancel_id,
                            CancelResult::kCanceled});
  second.counter.EmitCancel({sent.order_id, sent.service_cancel_id,
                             CancelResult::kCanceled});
  first.counter.EmitCancel({sent.order_id, sent.service_cancel_id,
                            static_cast<CancelResult>(99)});
  EXPECT_TRUE(attempt.cancels.empty());
  EXPECT_EQ(owner.orders.size(), 1U);
  first.counter.EmitCancel({sent.order_id, sent.service_cancel_id,
                            CancelResult::kCanceled});
  ASSERT_EQ(attempt.cancels.size(), 1U);
  EXPECT_EQ(attempt.cancels[0].cancel_request_id, 11U);
}

TEST_F(CancellationTest, LateSuccessPreservesTheEarlierFailureSnapshot) {
  Open();
  first.counter.SetNextCancel({SendResult::kNotSent, std::nullopt});
  Cancel(Request(10), attempt);
  service.Run();
  ASSERT_EQ(attempt.failures.size(), 1U);
  ASSERT_TRUE(attempt.failures[0].snapshot);
  const auto version = attempt.failures[0].snapshot->version;
  const auto sent = first.requests[0];
  first.counter.EmitCancel({sent.order_id, sent.service_cancel_id,
                            CancelResult::kCanceled});
  ASSERT_EQ(attempt.cancels.size(), 1U);
  EXPECT_EQ(attempt.failures[0].snapshot->state, OrderState::kAccepted);
  EXPECT_EQ(attempt.cancels[0].snapshot.state, OrderState::kCanceled);
  EXPECT_GT(attempt.cancels[0].snapshot.version, version);
  EXPECT_EQ(first.requests.size(), 1U);
}

TEST_F(CancellationTest, LateAcceptanceCannotRestoreACanceledOrder) {
  first.counter.SetNextSubmit({SendResult::kSent, std::nullopt});
  EXPECT_EQ(client->SubmitOrder(submit, owner), RequestResult::kDelivered);
  Cancel(Request(1), attempt);
  service.Run();
  ASSERT_EQ(owner.orders.size(), 1U);
  EXPECT_EQ(owner.orders[0].snapshot.state, OrderState::kCanceled);
  first.counter.Emit({submit.order.order_id, AdapterOrderState::kAccepted});
  first.counter.Emit({submit.order.order_id, AdapterOrderState::kRejected});
  EXPECT_EQ(owner.orders.size(), 1U);
  Cancel(Request(2), other);
  service.Run();
  ASSERT_EQ(other.failures.size(), 1U);
  EXPECT_EQ(other.failures[0].reason, FailureReason::kOrderInactive);
  EXPECT_EQ(first.requests.size(), 1U);
}

TEST_F(CancellationTest, PendingOrderRejectionAlsoRejectsTheCancellation) {
  first.counter.SetNextSubmit(
      {SendResult::kSent, AdapterOrderState::kRejected});
  EXPECT_EQ(client->SubmitOrder(submit, owner), RequestResult::kDelivered);
  Cancel(Request(1), attempt);
  service.Run();
  ASSERT_EQ(owner.orders.size(), 1U);
  ASSERT_EQ(attempt.cancels.size(), 1U);
  EXPECT_EQ(owner.orders[0].snapshot.state, OrderState::kRejected);
  EXPECT_EQ(attempt.cancels[0].result, CancelResult::kRejected);
  EXPECT_EQ(attempt.cancels[0].snapshot.state, OrderState::kRejected);
}

TEST_F(CancellationTest, CallbackCanEnqueueRequestsAndKeepItsSnapshot) {
  Open();
  attempt.on_cancel = [&](const CancelReport& report) {
    const auto saved = report.snapshot;
    service.Run();
    for (std::uint64_t id = 1; id <= 300; ++id) {
      Cancel({{200, id}, 7, 2, id}, failures);
    }
    EXPECT_TRUE(failures.failures.empty());
    EXPECT_EQ(report.snapshot.order, saved.order);
    EXPECT_EQ(report.snapshot.version, saved.version);
    EXPECT_EQ(report.snapshot.state, saved.state);
  };
  Cancel(Request(1), attempt);
  service.Run();
  ASSERT_EQ(attempt.cancels.size(), 1U);
  while (failures.failures.size() < 300) {
    service.Run();
  }
  EXPECT_EQ(failures.failures.size(), 300U);
}

TEST_F(CancellationTest, CounterResultWaitsBehindQueuedFailureNotification) {
  Open();
  first.counter.SetNextCancel({SendResult::kNotSent, std::nullopt});
  Cancel(Request(12), attempt);
  owner.on_order = [&](const OrderReport&) {
    EXPECT_EQ(attempt.failures.size(), 1U);
    EXPECT_TRUE(attempt.cancels.empty());
  };
  const auto sent = first.requests[0];
  first.counter.EmitCancel({sent.order_id, sent.service_cancel_id,
                            CancelResult::kCanceled});
  EXPECT_TRUE(attempt.failures.empty());
  EXPECT_TRUE(attempt.cancels.empty());
  EXPECT_EQ(owner.orders.size(), 1U);
  service.Run();
  ASSERT_EQ(attempt.failures.size(), 1U);
  ASSERT_EQ(attempt.cancels.size(), 1U);
  ASSERT_TRUE(attempt.failures[0].snapshot);
  EXPECT_EQ(attempt.failures[0].snapshot->state, OrderState::kAccepted);
  EXPECT_EQ(attempt.cancels[0].snapshot.state, OrderState::kCanceled);
}

TEST_F(CancellationTest, ARejectedOrderCannotBeChangedByACancelSuccess) {
  first.counter.SetNextSubmit({SendResult::kSent, std::nullopt});
  EXPECT_EQ(client->SubmitOrder(submit, owner), RequestResult::kDelivered);
  first.counter.SetNextCancel({SendResult::kSent, std::nullopt});
  Cancel(Request(1), attempt);
  first.counter.Emit({submit.order.order_id, AdapterOrderState::kRejected});
  const auto sent = first.requests[0];
  first.counter.EmitCancel({sent.order_id, sent.service_cancel_id,
                            CancelResult::kCanceled});
  EXPECT_TRUE(attempt.cancels.empty());
  ASSERT_EQ(owner.orders.size(), 1U);
  EXPECT_EQ(owner.orders[0].snapshot.state, OrderState::kRejected);
  first.counter.EmitCancel({sent.order_id, sent.service_cancel_id,
                            CancelResult::kRejected});
  ASSERT_EQ(attempt.cancels.size(), 1U);
  EXPECT_EQ(attempt.cancels[0].snapshot.state, OrderState::kRejected);
}

// Checks the adapter callback timing without the service's notification queue.
class CounterResults final : public ITradingAdapterCallback {
 public:
  // ITradingAdapterCallback implementation.
  void OnOrderReport(const AdapterOrderReport&) override { ++orders; }
  void OnCancelReport(const AdapterCancelReport& report) override {
    EXPECT_FALSE(in_cancel);
    reports.push_back(report);
  }

  bool in_cancel = false;
  int orders = 0;
  std::vector<AdapterCancelReport> reports;
};

TEST(CancellationAdapterTest, DefersCallbacksAndMaintainsCanceledBookState) {
  SimulatedTradingAdapter adapter;
  CounterResults result;
  adapter.SetCallback(result);
  EXPECT_EQ(adapter.SubmitOrder(
                {{{100, 1}, 7, 2, 42, Side::kBuy, Offset::kOpen, 100, 1}}),
            SendResult::kSent);
  adapter.Run();
  EXPECT_EQ(result.orders, 1);
  result.in_cancel = true;
  EXPECT_EQ(adapter.CancelOrder({{100, 1}, 1}), SendResult::kSent);
  result.in_cancel = false;
  EXPECT_TRUE(result.reports.empty());
  adapter.Run();
  ASSERT_EQ(result.reports.size(), 1U);
  EXPECT_EQ(result.reports[0].result, CancelResult::kCanceled);
  EXPECT_EQ(result.reports[0].service_cancel_id, 1U);
  EXPECT_EQ(adapter.CancelOrder({{100, 1}, 2}), SendResult::kSent);
  adapter.Run();
  ASSERT_EQ(result.reports.size(), 2U);
  EXPECT_EQ(result.reports[1].result, CancelResult::kRejected);
}

TEST(CancellationAdapterTest, SharesBatchLimitAndAdvancesBothReportQueues) {
  SimulatedTradingAdapter adapter;
  CounterResults result;
  adapter.SetCallback(result);
  for (std::uint64_t seq = 1; seq <= 70; ++seq) {
    EXPECT_EQ(adapter.SubmitOrder(
                  {{{100, seq}, 7, 2, 42, Side::kBuy, Offset::kOpen, 100, 1}}),
              SendResult::kSent);
    EXPECT_EQ(adapter.CancelOrder({{100, seq}, seq}), SendResult::kSent);
  }
  EXPECT_EQ(result.orders, 0);
  EXPECT_TRUE(result.reports.empty());
  adapter.Run();
  EXPECT_EQ(result.orders, 32);
  EXPECT_EQ(result.reports.size(), 32U);
  adapter.Run();
  EXPECT_EQ(result.orders, 64);
  EXPECT_EQ(result.reports.size(), 64U);
  adapter.Run();
  EXPECT_EQ(result.orders, 70);
  EXPECT_EQ(result.reports.size(), 70U);
}

}  // namespace
}  // namespace yquant::trading
