#include <stddef.h>
#include <stdint.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/trading/mock_trading_adapter.h"

namespace yquant::trading {
namespace {

constexpr uint64_t kInterval = 1000000;

model::OrderRequest Request(int64_t quantity = 9) {
  return {{(1ULL << 32) | 600000}, model::OrderSide::kBuy, 1000, quantity};
}

bool Start(MockTradingAdapter& adapter,
           MockTradingMode mode = MockTradingMode::kFill) {
  TradingConfig config;
  config.mode = mode;
  return adapter.Prepare(config).ok() && adapter.Start(0).ok();
}

std::vector<std::byte> Snapshot(MockTradingAdapter& adapter, uint64_t now) {
  std::vector<std::byte> bytes(64 * 1024);
  size_t used = 0;
  if (!adapter.EncodeState(bytes, used, now).ok()) {
    return {};
  }
  bytes.resize(used);
  return bytes;
}

struct Observer {
  std::array<MockTradingMutation, 16> mutations;
  size_t size = 0;
  bool fail = false;

  static base::Status Observe(void* context,
                              const MockTradingMutation& mutation) noexcept {
    auto& self = *static_cast<Observer*>(context);
    if (self.fail) {
      return {base::ErrorCode::kIoError, {}};
    }
    if (self.size == self.mutations.size()) {
      return {base::ErrorCode::kResourceExhausted, {}};
    }
    self.mutations[self.size++] = mutation;
    return {};
  }
};

// Recomputes the snapshot trailer when exercising typed validation rather
// than merely checksum detection. The file format's checksum is FNV-1a/64.
void RefreshChecksum(std::span<std::byte> bytes) {
  uint64_t checksum = 14695981039346656037ULL;
  for (const auto byte : bytes.first(bytes.size() - 8)) {
    checksum ^= std::to_integer<uint8_t>(byte);
    checksum *= 1099511628211ULL;
  }
  for (size_t index = 0; index < 8; ++index) {
    bytes[bytes.size() - 8 + index] = static_cast<std::byte>(checksum & 0xffU);
    checksum >>= 8;
  }
}

TEST(MockTradingPersistenceTest, SnapshotRestoresPartialFillAndPendingCancel) {
  MockTradingAdapter source;
  ASSERT_TRUE(Start(source));
  ASSERT_TRUE(
      source.SendOrderWithIdentity({7}, Request(), "session-A/order-9", 0)
          .ok());
  TradingEvent event;
  ASSERT_EQ(source.Poll(kInterval, event).state, TradingPollState::kEvent);
  const auto broker = event.broker_order_id;
  ASSERT_TRUE(source.SendCancel({7}, broker, {11}, kInterval).ok());
  ASSERT_EQ(source.Poll(2 * kInterval, event).state, TradingPollState::kEvent);
  ASSERT_EQ(event.kind, model::OrderUpdateKind::kFill);
  ASSERT_EQ(event.fill.quantity, 4);
  const auto first_execution = event.fill.execution_id;
  const auto first_utc = event.fill.event_time_ns;
  auto bytes = Snapshot(source, 2 * kInterval);
  ASSERT_FALSE(bytes.empty());

  MockTradingAdapter recovered;
  ASSERT_TRUE(Start(recovered));
  uint64_t saved_now = 0;
  ASSERT_TRUE(recovered.RestoreState(bytes, 100 * kInterval, &saved_now).ok());
  EXPECT_EQ(saved_now, 2 * kInterval);
  EXPECT_EQ(recovered.next_deadline(), 100 * kInterval);
  EXPECT_EQ(recovered.recovery_order_count(), 1U);
  EXPECT_EQ(recovered.recovery_event_count(), 2U);
  EXPECT_EQ(recovered.recovery_fill_count(), 1U);
  model::OrderSnapshot order;
  model::BrokerOrderId key;
  ASSERT_TRUE(recovered.ReadRecoveryOrder(0, order, &key).ok());
  EXPECT_EQ(key.view(), "session-A/order-9");
  EXPECT_EQ(order.cumulative_filled_qty, 4);
  EXPECT_EQ(order.leaves_qty, 5);
  EXPECT_EQ(order.report_sequence, 2U);
  EXPECT_TRUE(order.cancel_pending);
  model::Fill fill;
  ASSERT_TRUE(recovered.ReadRecoveryFill(0, fill).ok());
  EXPECT_EQ(fill.execution_id, first_execution);
  EXPECT_EQ(fill.event_time_ns, first_utc);

  ASSERT_EQ(recovered.Poll(100 * kInterval, event).state,
            TradingPollState::kEvent);
  EXPECT_EQ(event.kind, model::OrderUpdateKind::kCanceled);
  EXPECT_EQ(event.cumulative_filled_qty, 4);
  EXPECT_EQ(event.report_sequence, 3U);
  EXPECT_EQ(event.cancel_request_id.value, 11U);
  EXPECT_EQ(recovered.recovery_fill_count(), 1U);
  EXPECT_TRUE(recovered.is_drained());
  EXPECT_EQ(recovered.Poll(200 * kInterval, event).state,
            TradingPollState::kIdle);
  ASSERT_TRUE(recovered.ReadRecoveryOrder(0, order).ok());
  EXPECT_EQ(order.leaves_qty, 0);
  EXPECT_EQ(order.state, model::OrderState::kCanceled);
}

TEST(MockTradingPersistenceTest, ReplayPreservesExactEventsAndSuppressesSink) {
  MockTradingAdapter source;
  ASSERT_TRUE(Start(source));
  Observer source_observer;
  source.SetMutationObserver(Observer::Observe, &source_observer);
  ASSERT_TRUE(
      source.SendOrderWithIdentity({1}, Request(1), "unique-request", 0).ok());
  TradingEvent event;
  ASSERT_EQ(source.Poll(kInterval, event).state, TradingPollState::kEvent);
  const auto broker = event.broker_order_id;
  ASSERT_EQ(source.Poll(2 * kInterval, event).state, TradingPollState::kEvent);
  const auto utc = event.fill.event_time_ns;
  ASSERT_TRUE(source.SendCancel({1}, broker, {3}, 2 * kInterval).ok());
  ASSERT_EQ(source.Poll(3 * kInterval, event).state, TradingPollState::kEvent);
  ASSERT_EQ(source_observer.size, 5U);

  MockTradingAdapter replay;
  ASSERT_TRUE(Start(replay));
  Observer rejected_observer;
  rejected_observer.fail = true;
  replay.SetMutationObserver(Observer::Observe, &rejected_observer);
  for (size_t index = 0; index < source_observer.size; ++index) {
    std::array<std::byte, kMaxMockTradingMutationBytes> bytes;
    size_t size = 0;
    ASSERT_TRUE(
        EncodeMockTradingMutation(source_observer.mutations[index], bytes, size)
            .ok());
    MockTradingMutation decoded;
    ASSERT_TRUE(
        DecodeMockTradingMutation(std::span(bytes).first(size), decoded).ok());
    ASSERT_TRUE(replay.ReplayMutation(decoded).ok());
  }
  EXPECT_EQ(rejected_observer.size, 0U);
  EXPECT_EQ(Snapshot(source, 3 * kInterval), Snapshot(replay, 3 * kInterval));
  model::Fill fill;
  ASSERT_TRUE(replay.ReadRecoveryFill(0, fill).ok());
  EXPECT_EQ(fill.event_time_ns, utc);
  EXPECT_EQ(replay.recovery_fill_count(), 1U);
  EXPECT_EQ(replay.recovery_event_count(), 3U);
  ASSERT_TRUE(replay.ReadRecoveryEvent(1, event).ok());
  EXPECT_EQ(event.kind, model::OrderUpdateKind::kFill);
  EXPECT_EQ(event.fill.event_time_ns, utc);
  ASSERT_TRUE(replay.ReadRecoveryEvent(2, event).ok());
  EXPECT_EQ(event.kind, model::OrderUpdateKind::kCancelRejected);
  model::OrderSnapshot order;
  ASSERT_TRUE(replay.ReadRecoveryOrder(0, order).ok());
  EXPECT_EQ(order.state, model::OrderState::kFilled);
  EXPECT_TRUE(replay.is_drained());
  EXPECT_FALSE(replay.ReplayMutation(source_observer.mutations[2]).ok());
  EXPECT_EQ(Snapshot(source, 3 * kInterval), Snapshot(replay, 3 * kInterval));
  // A reboot can place the new monotonic clock below every historical report.
  // The evidence retains its UTC and original receive time, with no new fill.
  MockTradingAdapter restored;
  ASSERT_TRUE(Start(restored));
  ASSERT_TRUE(restored.RestoreState(Snapshot(replay, 3 * kInterval), 0).ok());
  ASSERT_TRUE(restored.ReadRecoveryEvent(1, event).ok());
  EXPECT_EQ(event.fill.event_time_ns, utc);
  EXPECT_EQ(event.receive_time_ns, 2 * kInterval);
  EXPECT_TRUE(restored.is_drained());
  EXPECT_EQ(restored.recovery_fill_count(), 1U);
}

TEST(MockTradingPersistenceTest, RejectedJournalLeavesEveryOperationUnchanged) {
  MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter));
  Observer observer;
  adapter.SetMutationObserver(Observer::Observe, &observer);
  observer.fail = true;
  const auto empty = Snapshot(adapter, 0);
  EXPECT_FALSE(adapter.SendOrder({1}, Request(), 0).ok());
  EXPECT_EQ(Snapshot(adapter, 0), empty);
  observer.fail = false;
  ASSERT_TRUE(adapter.SendOrder({1}, Request(), 0).ok());
  const auto before_report = Snapshot(adapter, kInterval);
  observer.fail = true;
  TradingEvent event;
  event.client_order_id = {99};
  EXPECT_EQ(adapter.Poll(kInterval, event).state, TradingPollState::kError);
  EXPECT_EQ(event.client_order_id.value, 99U);
  EXPECT_EQ(Snapshot(adapter, kInterval), before_report);
  observer.fail = false;
  ASSERT_EQ(adapter.Poll(kInterval, event).state, TradingPollState::kEvent);
  const auto accepted = Snapshot(adapter, kInterval);
  observer.fail = true;
  EXPECT_FALSE(
      adapter.SendCancel({1}, event.broker_order_id, {8}, kInterval).ok());
  EXPECT_EQ(Snapshot(adapter, kInterval), accepted);
  observer.fail = false;
  EXPECT_TRUE(
      adapter.SendCancel({1}, event.broker_order_id, {8}, kInterval).ok());
}

TEST(MockTradingPersistenceTest, RejectsDamagedAndInvalidSnapshotAtomically) {
  MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter));
  ASSERT_TRUE(adapter.SendOrder({1}, Request(), 0).ok());
  const auto before = Snapshot(adapter, 0);
  ASSERT_FALSE(before.empty());
  auto damaged = before;
  damaged[0] ^= std::byte{1};
  uint64_t saved_now = 999;
  EXPECT_EQ(adapter.RestoreState(damaged, kInterval, &saved_now).code,
            base::ErrorCode::kCorrupt);
  EXPECT_EQ(saved_now, 999U);
  EXPECT_EQ(Snapshot(adapter, 0), before);
  EXPECT_FALSE(
      adapter
          .RestoreState(std::span(before).first(before.size() - 1), kInterval)
          .ok());

  // With no positions, order_count begins at byte 45. A valid checksum must
  // not permit a count beyond the public 1024-order capacity.
  damaged = before;
  damaged[45] = std::byte{1};
  damaged[46] = std::byte{4};
  RefreshChecksum(damaged);
  EXPECT_EQ(adapter.RestoreState(damaged, kInterval).code,
            base::ErrorCode::kCorrupt);
  EXPECT_EQ(Snapshot(adapter, 0), before);

  // The first order's state follows variable-length broker/key fields.
  damaged = before;
  constexpr size_t kFirstOrderOffset = 65;
  const auto broker_size =
      std::to_integer<uint8_t>(damaged[kFirstOrderOffset + 8]);
  const auto key_size_offset = kFirstOrderOffset + 9 + broker_size;
  const auto key_size = std::to_integer<uint8_t>(damaged[key_size_offset]);
  const auto state_offset = key_size_offset + 1 + key_size + 25;
  damaged[state_offset] = std::byte{255};
  RefreshChecksum(damaged);
  EXPECT_EQ(adapter.RestoreState(damaged, kInterval).code,
            base::ErrorCode::kCorrupt);
  EXPECT_EQ(Snapshot(adapter, 0), before);
}

TEST(MockTradingPersistenceTest, ValidatesMutationAndExternalIdentity) {
  MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter));
  EXPECT_FALSE(
      adapter.SendOrderWithIdentity({1}, Request(), std::string(65, 'x'), 0)
          .ok());
  ASSERT_TRUE(adapter.SendOrderWithIdentity({1}, Request(), "a", 0).ok());
  EXPECT_FALSE(adapter.SendOrderWithIdentity({2}, Request(), "a", 0).ok());
  EXPECT_EQ(adapter.recovery_order_count(), 1U);
  MockTradingMutation mutation;
  mutation.client_order_id = {2};
  mutation.request = Request();
  std::array<std::byte, kMaxMockTradingMutationBytes> bytes;
  size_t used = 123;
  EXPECT_FALSE(
      EncodeMockTradingMutation(mutation, std::span(bytes).first(1), used)
          .ok());
  EXPECT_EQ(used, 123U);
  ASSERT_TRUE(EncodeMockTradingMutation(mutation, bytes, used).ok());
  bytes[8] ^= std::byte{1};
  MockTradingMutation output;
  output.client_order_id = {999};
  EXPECT_EQ(
      DecodeMockTradingMutation(std::span(bytes).first(used), output).code,
      base::ErrorCode::kCorrupt);
  EXPECT_EQ(output.client_order_id.value, 999U);
}

TEST(MockTradingPersistenceTest, RebaseChecksOverflowBeforeChangingPlans) {
  MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter));
  ASSERT_TRUE(adapter.SendOrder({1}, Request(), 0).ok());
  const auto before = Snapshot(adapter, 0);
  EXPECT_EQ(adapter.RebaseDeadlines(0, UINT64_MAX).code,
            base::ErrorCode::kResourceExhausted);
  EXPECT_EQ(Snapshot(adapter, 0), before);
  ASSERT_TRUE(adapter.RebaseDeadlines(0, 100 * kInterval).ok());
  EXPECT_EQ(adapter.next_deadline(), 101 * kInterval);
  ASSERT_TRUE(adapter.RebaseDeadlines(200 * kInterval, 300 * kInterval).ok());
  EXPECT_EQ(adapter.next_deadline(), 300 * kInterval);
}

TEST(MockTradingPersistenceTest, RestoreUsesSavedBootstrapInsteadOfNewConfig) {
  TradingConfig original;
  original.initial_account.cash_balance = 1000000000;
  original.initial_account.position_count = 1;
  original.initial_account.positions[0] = {Request().instrument_id, 100, 80, 0};
  MockTradingAdapter source;
  ASSERT_TRUE(source.Prepare(original).ok());
  ASSERT_TRUE(source.Start(0).ok());
  const auto checkpoint = Snapshot(source, 0);
  TradingConfig changed;
  changed.initial_account.cash_balance = 777;
  MockTradingAdapter restored;
  ASSERT_TRUE(restored.Prepare(changed).ok());
  ASSERT_TRUE(restored.Start(0).ok());
  ASSERT_TRUE(restored.RestoreState(checkpoint, kInterval).ok());
  TradingAccountSnapshot account;
  ASSERT_TRUE(restored.ReadAccountSnapshot(account).ok());
  EXPECT_EQ(account.cash_balance, 1000000000);
  EXPECT_EQ(account.position_count, 1U);
  EXPECT_EQ(account.positions[0].total_qty, 100);
  EXPECT_EQ(account.positions[0].available_qty, 80);
  EXPECT_EQ(Snapshot(restored, 0), checkpoint);
}

TEST(MockTradingPersistenceTest, InvalidReplayReportPreservesPendingPlan) {
  MockTradingAdapter source;
  ASSERT_TRUE(Start(source));
  Observer observer;
  source.SetMutationObserver(Observer::Observe, &observer);
  ASSERT_TRUE(source.SendOrder({1}, Request(), 0).ok());
  TradingEvent event;
  ASSERT_EQ(source.Poll(kInterval, event).state, TradingPollState::kEvent);
  MockTradingAdapter replay;
  ASSERT_TRUE(Start(replay));
  ASSERT_TRUE(replay.ReplayMutation(observer.mutations[0]).ok());
  const auto before = Snapshot(replay, kInterval);
  auto damaged = observer.mutations[1];
  damaged.event.report_sequence = 2;
  EXPECT_FALSE(replay.ReplayMutation(damaged).ok());
  EXPECT_EQ(Snapshot(replay, kInterval), before);
  EXPECT_TRUE(replay.ReplayMutation(observer.mutations[1]).ok());
}

TEST(MockTradingPersistenceTest, RejectsUnrepresentablePositionEffects) {
  MockTradingMutation mutation;
  mutation.client_order_id = {1};
  mutation.request = Request();
  mutation.request.position_effect = model::PositionEffect::kOpen;
  std::array<std::byte, kMaxMockTradingMutationBytes> bytes;
  size_t used = 123;
  EXPECT_EQ(EncodeMockTradingMutation(mutation, bytes, used).code,
            base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(used, 123U);

  MockTradingAdapter source;
  ASSERT_TRUE(Start(source));
  Observer observer;
  source.SetMutationObserver(Observer::Observe, &observer);
  EXPECT_EQ(source.SendOrder({1}, mutation.request, 0).code,
            base::ErrorCode::kUnsupported);
  EXPECT_EQ(source.recovery_order_count(), 0U);
  EXPECT_EQ(observer.size, 0U);
  source.SetMutationObserver(nullptr, nullptr);
  EXPECT_EQ(source.SendOrderWithIdentity({1}, mutation.request, "key", 0).code,
            base::ErrorCode::kUnsupported);
  // Preserve the standalone mock's existing request behavior. Its state cannot
  // be encoded as cash-only persistence while it contains such a request.
  ASSERT_TRUE(source.SendOrder({1}, mutation.request, 0).ok());
  std::vector<std::byte> state(kMaxMockTradingStateBytes);
  EXPECT_EQ(source.EncodeState(state, used, 0).code,
            base::ErrorCode::kUnsupported);
  EXPECT_EQ(used, 123U);
}

TEST(MockTradingPersistenceTest, RejectsFuturesAccountPersistence) {
  MockTradingAdapter cash;
  ASSERT_TRUE(Start(cash));
  const auto cash_snapshot = Snapshot(cash, 0);
  ASSERT_FALSE(cash_snapshot.empty());

  TradingConfig config;
  config.adapter = TradingAdapterKind::kSimulatedFutures;
  config.initial_account.type = model::AccountType::kFutures;
  MockTradingAdapter futures;
  ASSERT_TRUE(futures.Prepare(config).ok());
  ASSERT_TRUE(futures.Start(0).ok());
  std::vector<std::byte> state(kMaxMockTradingStateBytes);
  size_t used = 123;
  EXPECT_EQ(futures.EncodeState(state, used, 0).code,
            base::ErrorCode::kUnsupported);
  EXPECT_EQ(used, 123U);
  uint64_t saved_now = 456;
  EXPECT_EQ(futures.RestoreState(cash_snapshot, 0, &saved_now).code,
            base::ErrorCode::kUnsupported);
  EXPECT_EQ(saved_now, 456U);
  MockTradingMutation mutation;
  mutation.client_order_id = {1};
  mutation.request = Request();
  EXPECT_EQ(futures.ReplayMutation(mutation).code,
            base::ErrorCode::kUnsupported);
  TradingAccountSnapshot account;
  ASSERT_TRUE(futures.ReadAccountSnapshot(account).ok());
  EXPECT_EQ(account.type, model::AccountType::kFutures);
}

TEST(MockTradingPersistenceTest, ReportCodecRejectsFuturesExecutionEffect) {
  MockTradingAdapter source;
  ASSERT_TRUE(Start(source));
  Observer observer;
  source.SetMutationObserver(Observer::Observe, &observer);
  ASSERT_TRUE(source.SendOrder({1}, Request(), 0).ok());
  TradingEvent event;
  ASSERT_EQ(source.Poll(kInterval, event).state, TradingPollState::kEvent);
  ASSERT_EQ(source.Poll(2 * kInterval, event).state, TradingPollState::kEvent);
  ASSERT_EQ(event.kind, model::OrderUpdateKind::kFill);
  auto mutation = observer.mutations[2];
  mutation.event.fill.position_effect = model::PositionEffect::kCloseToday;
  std::array<std::byte, kMaxMockTradingMutationBytes> bytes;
  size_t used = 123;
  EXPECT_EQ(EncodeMockTradingMutation(mutation, bytes, used).code,
            base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(used, 123U);
}

}  // namespace
}  // namespace yquant::trading
