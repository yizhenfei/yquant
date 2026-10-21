#include "strategyd/trading_persistence.h"

#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "strategyd/trading.h"

namespace yquant::strategyd {
namespace {
using base::ErrorCode;

class Clock final : public MonotonicClock {
 public:
  std::uint64_t Now() const noexcept override { return now; }
  std::uint64_t now = 1000;
};

class Adapter final : public TradingAdapter {
 public:
  base::Status Prepare(const TradingConfig&) noexcept override { return {}; }
  base::Status Start(std::uint64_t) noexcept override { return {}; }
  base::Status ReadAccountSnapshot(TradingAccountSnapshot&) noexcept override {
    return {};
  }
  base::Status SendOrder(model::ClientOrderId, const model::OrderRequest&,
                         std::uint64_t) noexcept override {
    ++sends;
    return {};
  }
  base::Status SendCancel(model::ClientOrderId, const model::BrokerOrderId&,
                          model::CancelRequestId,
                          std::uint64_t) noexcept override {
    ++sends;
    return {};
  }
  TradingPollResult Poll(std::uint64_t, TradingEvent&) noexcept override {
    return {};
  }
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    return {};
  }
  bool is_drained() const noexcept override { return true; }
  base::Status Stop() noexcept override { return {}; }
  const char* error_detail() const noexcept override { return ""; }
  std::size_t sends = 0;
};

// Captures copies so tests can replay after the original callback returns.
class Sink final : public TradingMutationSink {
 public:
  base::Status BeforeCommit(const TradingMutation& fact) noexcept override {
    if (fail) return {ErrorCode::kIoError, {}};
    facts.push_back(fact);
    return {};
  }
  bool fail = false;
  std::vector<TradingMutation> facts;
};

class TradingPersistenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config.source_id = {1};
    config.interval_ns = 1000000;
    config.instrument_count = 1;
    auto& instrument = config.instruments[0].instrument;
    ASSERT_EQ(model::MakeInstrumentId("SSE", "600000", instrument.id),
              ErrorCode::kOk);
    instrument.venue = model::Venue::kSse;
    std::memcpy(instrument.symbol, "600000", model::kSymbolLength);
    instrument.price_decimals = 2;
    config.instruments[0].bid_price = 1000;
    config.instruments[0].ask_price = 1001;
    config.instruments[0].bid_qty = 100;
    config.instruments[0].ask_qty = 100;
    config.instruments[0].price_step = 1;
    ASSERT_TRUE(market.Configure(config).ok());
    account.cash_balance = 1000000000;
    account.position_count = 1;
    account.positions[0] = {instrument.id, 1000, 1000, 0};
    ASSERT_TRUE(ledger.Configure(account).ok());
    ledger.SetMutationSink(&sink);
    ledger.SetRunning(true);
  }
  model::ClientOrderId Submit(std::int64_t quantity = 100) {
    model::ClientOrderId id;
    ledger.BeginCallback(CallbackPhase::kTick);
    EXPECT_EQ(ledger.SubmitOrder({config.instruments[0].instrument.id,
                                  model::OrderSide::kBuy, 1000, quantity},
                                 id),
              ErrorCode::kOk);
    ledger.EndCallback();
    return id;
  }
  TradingEvent Report(model::ClientOrderId id, std::uint64_t seq,
                      model::OrderUpdateKind kind) const {
    TradingEvent report;
    report.client_order_id = id;
    EXPECT_EQ(report.broker_order_id.Assign("broker-1"), ErrorCode::kOk);
    report.report_sequence = seq;
    report.kind = kind;
    report.receive_time_ns = clock.now;
    return report;
  }
  TradingEvent Fill(model::ClientOrderId id, std::uint64_t seq = 2) const {
    auto report = Report(id, seq, model::OrderUpdateKind::kFill);
    report.cumulative_filled_qty = 30;
    report.fill.client_order_id = id;
    report.fill.broker_order_id = report.broker_order_id;
    EXPECT_EQ(report.fill.execution_id.Assign("fill-1"), ErrorCode::kOk);
    report.fill.instrument_id = config.instruments[0].instrument.id;
    report.fill.side = model::OrderSide::kBuy;
    report.fill.price = 999;
    report.fill.quantity = 30;
    report.fill.receive_time_ns = clock.now;
    return report;
  }
  model::OrderSnapshot Order(TradingImpl& trading,
                             model::ClientOrderId id) const {
    model::OrderSnapshot out;
    EXPECT_TRUE(trading.TryGetOrder(id, out));
    return out;
  }
  model::CashSnapshot Cash(TradingImpl& trading) const {
    model::CashSnapshot out;
    EXPECT_TRUE(trading.TryGetCash(out));
    return out;
  }
  std::vector<std::byte> Checkpoint(TradingImpl& trading) const {
    std::vector<std::byte> bytes(kMaxTradingCheckpointBytes);
    std::size_t size = 0;
    EXPECT_TRUE(trading.ExportCheckpoint(bytes, size).ok());
    bytes.resize(size);
    return bytes;
  }
  Clock clock;
  MarketDataConfig config;
  TradingAccountSnapshot account;
  MarketDataImpl market{clock};
  TradingImpl ledger{clock, market};
  Sink sink;
  Adapter adapter;
};

TEST_F(TradingPersistenceTest, RejectedJournalPreservesOrderAndSendQueues) {
  sink.fail = true;
  const auto cash = Cash(ledger);
  ledger.BeginCallback(CallbackPhase::kTick);
  model::ClientOrderId id{999};
  EXPECT_EQ(ledger.SubmitOrder({config.instruments[0].instrument.id,
                                model::OrderSide::kBuy, 1000, 100},
                               id),
            ErrorCode::kIoError);
  ledger.EndCallback();
  EXPECT_EQ(id.value, 999U);
  EXPECT_EQ(ledger.order_count(), 0U);
  EXPECT_EQ(Cash(ledger), cash);
  EXPECT_EQ(ledger.mutation_revision(), 0U);
  sink.fail = false;
  id = Submit();
  sink.fail = true;
  EXPECT_EQ(ledger.ProcessRequests(adapter, clock.now).code,
            ErrorCode::kIoError);
  EXPECT_EQ(adapter.sends, 0U);
  EXPECT_EQ(ledger.mutation_revision(), 1U);
  sink.fail = false;
  EXPECT_TRUE(ledger.ProcessRequests(adapter, clock.now).ok());
  EXPECT_EQ(adapter.sends, 1U);
  EXPECT_EQ(sink.facts.back().kind, TradingMutationKind::kSendNew);
  EXPECT_EQ(sink.facts.back().order_id, id);
}

TEST_F(TradingPersistenceTest, RejectedJournalPreservesReportAndOutput) {
  const auto id = Submit();
  ASSERT_TRUE(ledger.ProcessRequests(adapter, clock.now).ok());
  TradingDispatch dispatch;
  ASSERT_EQ(ledger.ApplyEvent(Report(id, 1, model::OrderUpdateKind::kAccepted),
                              dispatch),
            ErrorCode::kOk);
  const auto old = Order(ledger, id);
  const auto cash = Cash(ledger);
  dispatch.update.order.client_order_id.value = 999;
  sink.fail = true;
  EXPECT_EQ(ledger.ApplyEvent(Fill(id), dispatch), ErrorCode::kIoError);
  EXPECT_EQ(Order(ledger, id), old);
  EXPECT_EQ(Cash(ledger), cash);
  EXPECT_EQ(dispatch.update.order.client_order_id.value, 999U);
  sink.fail = false;
  ASSERT_EQ(ledger.ApplyEvent(Fill(id), dispatch), ErrorCode::kOk);
  EXPECT_TRUE(dispatch.has_fill);
  EXPECT_EQ(Cash(ledger).reserved, 700000000);
}

TEST_F(TradingPersistenceTest,
       CheckpointRetainsPartialFillReservationsAndDedup) {
  const auto id = Submit();
  ASSERT_TRUE(ledger.ProcessRequests(adapter, clock.now).ok());
  TradingDispatch dispatch;
  ASSERT_EQ(ledger.ApplyEvent(Report(id, 1, model::OrderUpdateKind::kAccepted),
                              dispatch),
            ErrorCode::kOk);
  ASSERT_EQ(ledger.ApplyEvent(Fill(id), dispatch), ErrorCode::kOk);
  ledger.BeginCallback(CallbackPhase::kTick);
  ASSERT_EQ(ledger.CancelOrder(id), ErrorCode::kOk);
  ledger.EndCallback();
  const auto bytes = Checkpoint(ledger);
  TradingImpl recovered{clock, market};
  ASSERT_TRUE(recovered.RestoreCheckpoint(bytes).ok());
  EXPECT_EQ(Order(recovered, id), Order(ledger, id));
  EXPECT_EQ(Cash(recovered), Cash(ledger));
  EXPECT_EQ(Cash(recovered).balance, 700300000);
  EXPECT_EQ(Cash(recovered).reserved, 700000000);
  EXPECT_EQ(recovered.mutation_revision(), ledger.mutation_revision());
  EXPECT_EQ(Checkpoint(recovered), bytes);
  ASSERT_EQ(recovered.ApplyEvent(Fill(id), dispatch), ErrorCode::kOk);
  EXPECT_FALSE(dispatch.has_fill);
  auto duplicate = Fill(id, 3);
  duplicate.cumulative_filled_qty = 60;
  EXPECT_EQ(recovered.ApplyEvent(duplicate, dispatch), ErrorCode::kCorrupt);
  recovered.AbandonRecoveredRequests();
  EXPECT_TRUE(recovered.ProcessRequests(adapter, clock.now).ok());
  EXPECT_EQ(adapter.sends, 1U);
  EXPECT_TRUE(Order(recovered, id).cancel_pending);
}

TEST_F(TradingPersistenceTest, ReplayRestoresFactsWithoutExternalCalls) {
  const auto id = Submit();
  ASSERT_TRUE(ledger.ProcessRequests(adapter, clock.now).ok());
  TradingDispatch dispatch;
  ASSERT_EQ(ledger.ApplyEvent(Report(id, 1, model::OrderUpdateKind::kAccepted),
                              dispatch),
            ErrorCode::kOk);
  ASSERT_EQ(ledger.ApplyEvent(Fill(id), dispatch), ErrorCode::kOk);
  TradingImpl replay{clock, market};
  ASSERT_TRUE(replay.Configure(account).ok());
  Sink replay_sink;
  replay.SetMutationSink(&replay_sink);
  clock.now = 1;  // Replay uses the original monotonic clock domain.
  for (const auto& fact : sink.facts) {
    std::array<std::byte, kMaxEncodedTradingMutation> bytes;
    std::size_t size = 0;
    ASSERT_TRUE(EncodeTradingMutation(fact, bytes, size).ok());
    TradingMutation decoded;
    ASSERT_TRUE(
        DecodeTradingMutation(std::span(bytes).first(size), decoded).ok());
    ASSERT_TRUE(replay.ReplayMutation(decoded, &dispatch).ok());
  }
  EXPECT_TRUE(dispatch.has_fill);
  EXPECT_TRUE(replay_sink.facts.empty());
  EXPECT_EQ(Order(replay, id), Order(ledger, id));
  EXPECT_EQ(Cash(replay), Cash(ledger));
  EXPECT_EQ(replay.mutation_revision(), 4U);
  EXPECT_EQ(adapter.sends, 1U);
  EXPECT_TRUE(replay.ProcessRequests(adapter, clock.now).ok());
  EXPECT_EQ(adapter.sends, 1U);
}

TEST_F(TradingPersistenceTest,
       AbandonedQueuedNewRemainsUncertainWithoutResend) {
  const auto id = Submit();
  TradingImpl recovered{clock, market};
  ASSERT_TRUE(recovered.RestoreCheckpoint(Checkpoint(ledger)).ok());
  recovered.AbandonRecoveredRequests();
  EXPECT_TRUE(recovered.ProcessRequests(adapter, clock.now).ok());
  EXPECT_EQ(adapter.sends, 0U);
  EXPECT_EQ(Order(recovered, id).state, model::OrderState::kPendingNew);
  EXPECT_EQ(Cash(recovered).reserved, 1000000000);
  TradingDispatch dispatch;
  EXPECT_EQ(recovered.ApplyEvent(
                Report(id, 1, model::OrderUpdateKind::kAccepted), dispatch),
            ErrorCode::kOk);
}

TEST_F(TradingPersistenceTest,
       LocalCancellationHooksReplayBeforeReservationRelease) {
  const auto id = Submit(10);
  ledger.BeginStopping();
  sink.fail = true;
  EXPECT_EQ(ledger.ProcessRequests(adapter, clock.now).code,
            ErrorCode::kIoError);
  EXPECT_FALSE(ledger.has_local_updates());
  sink.fail = false;
  ASSERT_TRUE(ledger.ProcessRequests(adapter, clock.now).ok());
  EXPECT_EQ(adapter.sends, 0U);
  EXPECT_TRUE(ledger.has_local_updates());
  const auto pending = Checkpoint(ledger);
  TradingImpl checkpoint_recovered{clock, market};
  ASSERT_TRUE(checkpoint_recovered.RestoreCheckpoint(pending).ok());
  EXPECT_TRUE(checkpoint_recovered.has_local_updates());
  sink.fail = true;
  TradingDispatch dispatch;
  EXPECT_EQ(ledger.ApplyLocalUpdate(dispatch), ErrorCode::kIoError);
  EXPECT_EQ(Order(ledger, id).state, model::OrderState::kPendingNew);
  EXPECT_EQ(Cash(ledger).reserved, 100000000);
  EXPECT_TRUE(ledger.has_local_updates());
  sink.fail = false;
  ASSERT_EQ(ledger.ApplyLocalUpdate(dispatch), ErrorCode::kOk);
  EXPECT_EQ(Cash(ledger).reserved, 0);
  EXPECT_EQ(Order(ledger, id).state, model::OrderState::kCanceled);
  TradingImpl recovered{clock, market};
  ASSERT_TRUE(recovered.Configure(account).ok());
  for (const auto& fact : sink.facts)
    ASSERT_TRUE(recovered.ReplayMutation(fact, &dispatch).ok());
  EXPECT_EQ(dispatch.update.kind, model::OrderUpdateKind::kCanceledBeforeSend);
  EXPECT_EQ(Order(recovered, id), Order(ledger, id));
  EXPECT_EQ(Cash(recovered), Cash(ledger));
}

TEST_F(TradingPersistenceTest, CancelHookFailureDoesNotConsumeIdentityOrQueue) {
  const auto id = Submit(10);
  ASSERT_TRUE(ledger.ProcessRequests(adapter, clock.now).ok());
  TradingDispatch dispatch;
  ASSERT_EQ(ledger.ApplyEvent(Report(id, 1, model::OrderUpdateKind::kAccepted),
                              dispatch),
            ErrorCode::kOk);
  sink.fail = true;
  ledger.BeginCallback(CallbackPhase::kTick);
  EXPECT_EQ(ledger.CancelOrder(id), ErrorCode::kIoError);
  ledger.EndCallback();
  EXPECT_FALSE(Order(ledger, id).cancel_pending);
  EXPECT_EQ(Order(ledger, id).cancel_request_id.value, 0U);
  sink.fail = false;
  ledger.BeginCallback(CallbackPhase::kTick);
  EXPECT_EQ(ledger.CancelOrder(id), ErrorCode::kOk);
  ledger.EndCallback();
  EXPECT_EQ(Order(ledger, id).cancel_request_id.value, 1U);
  sink.fail = true;
  EXPECT_EQ(ledger.ProcessRequests(adapter, clock.now).code,
            ErrorCode::kIoError);
  EXPECT_EQ(adapter.sends, 1U);
  sink.fail = false;
  ASSERT_TRUE(ledger.ProcessRequests(adapter, clock.now).ok());
  EXPECT_EQ(adapter.sends, 2U);
  TradingImpl recovered{clock, market};
  ASSERT_TRUE(recovered.Configure(account).ok());
  for (const auto& fact : sink.facts)
    ASSERT_TRUE(recovered.ReplayMutation(fact).ok());
  EXPECT_EQ(Order(recovered, id), Order(ledger, id));
  EXPECT_EQ(Checkpoint(recovered), Checkpoint(ledger));
}

TEST_F(TradingPersistenceTest, StableIdsAreIndependentOfStorageSlots) {
  ledger.SetRunning(false);
  ASSERT_TRUE(ledger.SetNextOrderIdAtLeast(1000000).ok());
  ledger.SetRunning(true);
  const auto id = Submit(10);
  EXPECT_EQ(id.value, 1000000U);
  model::OrderSnapshot order;
  EXPECT_TRUE(ledger.TryGetOrderAt(0, order));
  EXPECT_EQ(order.client_order_id, id);
  EXPECT_TRUE(ledger.TryGetOrder(id, order));
  EXPECT_FALSE(ledger.TryGetOrder({1}, order));
  TradingImpl recovered{clock, market};
  ASSERT_TRUE(recovered.RestoreCheckpoint(Checkpoint(ledger)).ok());
  EXPECT_EQ(recovered.next_order_id(), 1000001U);
  EXPECT_TRUE(recovered.TryGetOrder(id, order));
  ASSERT_TRUE(recovered.SetNextOrderIdAtLeast(2000000).ok());
  EXPECT_TRUE(recovered.SetNextOrderIdAtLeast(1).ok());
  EXPECT_EQ(recovered.next_order_id(), 2000000U);
  ASSERT_TRUE(recovered.SetNextCancelIdAtLeast(1000000).ok());
  EXPECT_EQ(recovered.next_cancel_id(), 1000000U);
  EXPECT_TRUE(recovered.SetNextCancelIdAtLeast(1).ok());
  EXPECT_EQ(recovered.next_cancel_id(), 1000000U);
  TradingImpl again{clock, market};
  ASSERT_TRUE(again.RestoreCheckpoint(Checkpoint(recovered)).ok());
  EXPECT_EQ(again.next_cancel_id(), 1000000U);
}

TEST_F(TradingPersistenceTest,
       ReconciledBaselineAlreadyIncludesHistoricalFills) {
  const auto id = Submit();
  ASSERT_TRUE(ledger.ProcessRequests(adapter, clock.now).ok());
  TradingDispatch dispatch;
  ASSERT_EQ(ledger.ApplyEvent(Report(id, 1, model::OrderUpdateKind::kAccepted),
                              dispatch),
            ErrorCode::kOk);
  ASSERT_EQ(ledger.ApplyEvent(Fill(id), dispatch), ErrorCode::kOk);
  auto terminal = Order(ledger, id);
  terminal.client_order_id = {1000000};
  terminal.state = model::OrderState::kCanceled;
  terminal.leaves_qty = 0;
  terminal.report_sequence = 3;
  auto historical_fill = Fill(id).fill;
  historical_fill.client_order_id = terminal.client_order_id;
  TradingAccountSnapshot baseline;
  baseline.cash_balance = 700300000;
  baseline.position_count = 1;
  baseline.positions[0] = {terminal.instrument_id, 1030, 1000, 0};
  TradingImpl recovered{clock, market};
  ASSERT_TRUE(recovered.Configure(account).ok());
  ASSERT_TRUE(recovered.SetNextOrderIdAtLeast(2000000).ok());
  ASSERT_TRUE(recovered.SetNextCancelIdAtLeast(1000).ok());
  ASSERT_TRUE(recovered
                  .RestoreReconciled(baseline, std::span(&terminal, 1),
                                     std::span(&historical_fill, 1))
                  .ok());
  EXPECT_EQ(Cash(recovered).balance, 700300000);
  EXPECT_EQ(Cash(recovered).reserved, 0);
  EXPECT_EQ(Cash(recovered).available, 700300000);
  model::PositionSnapshot position;
  ASSERT_TRUE(recovered.TryGetPosition(terminal.instrument_id, position));
  EXPECT_EQ(position.total_qty, 1030);
  EXPECT_EQ(position.available_qty, 1000);
  EXPECT_EQ(recovered.next_order_id(), 2000000U);
  EXPECT_EQ(recovered.next_cancel_id(), 1000U);
  EXPECT_EQ(recovered.mutation_revision(), 0U);
  auto duplicate = Fill(id);
  duplicate.client_order_id = terminal.client_order_id;
  duplicate.fill.client_order_id = terminal.client_order_id;
  EXPECT_EQ(recovered.ApplyEvent(duplicate, dispatch), ErrorCode::kOk);
  EXPECT_FALSE(dispatch.has_fill);
  EXPECT_EQ(Cash(recovered).balance, 700300000);
  TradingImpl checkpoint_restored{clock, market};
  ASSERT_TRUE(
      checkpoint_restored.RestoreCheckpoint(Checkpoint(recovered)).ok());
  EXPECT_EQ(Cash(checkpoint_restored), Cash(recovered));
}

TEST_F(TradingPersistenceTest, InvalidReconciledFactsPreserveExistingLedger) {
  const auto id = Submit(10);
  ledger.SetRunning(false);
  const auto before = Checkpoint(ledger);
  auto terminal = Order(ledger, id);
  terminal.state = model::OrderState::kFilled;
  terminal.cumulative_filled_qty = 10;
  terminal.leaves_qty = 0;
  terminal.report_sequence = 1;
  ASSERT_EQ(terminal.broker_order_id.Assign("broker-1"), ErrorCode::kOk);
  auto historical = Fill(id).fill;
  historical.quantity = 10;
  auto baseline = account;
  baseline.cash_balance = 900100000;
  baseline.positions[0].total_qty = 1010;
  auto restore = [&]() {
    return ledger.RestoreReconciled(baseline, std::span(&terminal, 1),
                                    std::span(&historical, 1));
  };
  baseline.positions[0].available_qty = 1011;
  EXPECT_EQ(restore().code, ErrorCode::kCorrupt);
  EXPECT_EQ(Checkpoint(ledger), before);
  baseline.positions[0].available_qty = 1000;
  historical.price = 1001;  // A buy execution cannot exceed its limit.
  EXPECT_EQ(restore().code, ErrorCode::kCorrupt);
  EXPECT_EQ(Checkpoint(ledger), before);
  historical.price = 999;
  baseline.positions[0].reserved_qty = 1;
  EXPECT_EQ(restore().code, ErrorCode::kCorrupt);
  EXPECT_EQ(Checkpoint(ledger), before);
  baseline.positions[0].reserved_qty = 0;
  terminal.cancel_pending = true;
  EXPECT_EQ(restore().code, ErrorCode::kCorrupt);
  EXPECT_EQ(Checkpoint(ledger), before);
  terminal.cancel_pending = false;
  const std::array duplicates = {historical, historical};
  EXPECT_EQ(
      ledger.RestoreReconciled(baseline, std::span(&terminal, 1), duplicates)
          .code,
      ErrorCode::kCorrupt);
  EXPECT_EQ(Checkpoint(ledger), before);
}

TEST_F(TradingPersistenceTest, ExhaustedColdOrderLeaseRejectsWithoutMutation) {
  const auto cash = Cash(ledger);
  EXPECT_EQ(ledger.SetNextOrderIdAtLeast(1000).code, ErrorCode::kInvalidState);
  EXPECT_EQ(ledger.next_order_id(), 1U);
  ledger.SetRunning(false);
  ASSERT_TRUE(ledger.SetNextOrderIdAtLeast(UINT64_MAX).ok());
  ledger.SetRunning(true);
  ledger.BeginCallback(CallbackPhase::kTick);
  model::ClientOrderId out{99};
  EXPECT_EQ(ledger.SubmitOrder({config.instruments[0].instrument.id,
                                model::OrderSide::kBuy, 1000, 10},
                               out),
            ErrorCode::kResourceExhausted);
  ledger.EndCallback();
  EXPECT_EQ(out.value, 99U);
  EXPECT_EQ(ledger.order_count(), 0U);
  EXPECT_EQ(ledger.mutation_revision(), 0U);
  EXPECT_EQ(Cash(ledger), cash);
  EXPECT_TRUE(sink.facts.empty());
}

TEST_F(TradingPersistenceTest, CancelIdentityNeverWrapsAfterFinalLeasedValue) {
  ledger.SetRunning(false);
  ASSERT_TRUE(ledger.SetNextCancelIdAtLeast(UINT64_MAX).ok());
  ledger.SetRunning(true);
  const auto id = Submit(10);
  ASSERT_TRUE(ledger.ProcessRequests(adapter, clock.now).ok());
  TradingDispatch dispatch;
  ASSERT_EQ(ledger.ApplyEvent(Report(id, 1, model::OrderUpdateKind::kAccepted),
                              dispatch),
            ErrorCode::kOk);
  ledger.BeginCallback(CallbackPhase::kTick);
  ASSERT_EQ(ledger.CancelOrder(id), ErrorCode::kOk);
  ledger.EndCallback();
  EXPECT_EQ(Order(ledger, id).cancel_request_id.value, UINT64_MAX);
  ASSERT_TRUE(ledger.ProcessRequests(adapter, clock.now).ok());
  auto rejected = Report(id, 2, model::OrderUpdateKind::kCancelRejected);
  rejected.cancel_request_id = {UINT64_MAX};
  ASSERT_EQ(ledger.ApplyEvent(rejected, dispatch), ErrorCode::kOk);
  const auto before = Checkpoint(ledger);
  ledger.BeginCallback(CallbackPhase::kTick);
  EXPECT_EQ(ledger.CancelOrder(id), ErrorCode::kResourceExhausted);
  ledger.EndCallback();
  EXPECT_EQ(Checkpoint(ledger), before);
  EXPECT_FALSE(Order(ledger, id).cancel_pending);
}

TEST_F(TradingPersistenceTest, CorruptCheckpointAndReplayPreserveLedger) {
  const auto id = Submit(10);
  auto bytes = Checkpoint(ledger);
  TradingImpl recovered{clock, market};
  ASSERT_TRUE(recovered.RestoreCheckpoint(bytes).ok());
  const auto before = Checkpoint(recovered);
  bytes[36] =
      std::byte{0xff};  // Cash balance contradicts derived availability.
  EXPECT_EQ(recovered.RestoreCheckpoint(bytes).code, ErrorCode::kCorrupt);
  EXPECT_EQ(Checkpoint(recovered), before);
  for (std::size_t size = 0; size < before.size(); ++size)
    EXPECT_EQ(recovered.RestoreCheckpoint(std::span(before).first(size)).code,
              ErrorCode::kCorrupt);
  auto fact = sink.facts[0];
  fact.revision = recovered.mutation_revision() + 1;
  fact.order_id = id;  // Already allocated identity must not be reused.
  EXPECT_EQ(recovered.ReplayMutation(fact).code, ErrorCode::kCorrupt);
  EXPECT_EQ(Checkpoint(recovered), before);
}
}  // namespace
}  // namespace yquant::strategyd
