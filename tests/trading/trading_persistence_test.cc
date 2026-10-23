#include "yquant/trading/trading_persistence.h"

#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "trading_engine_harness.h"
#include "yquant/trading/trading_engine.h"

namespace yquant::trading {
namespace {
using base::ErrorCode;

class Clock final : public base::MonotonicClock {
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
  base::Status SendCancel(model::ClientOrderId, const model::AdapterOrderId&,
                          model::CancelRequestId,
                          std::uint64_t) noexcept override {
    ++sends;
    return {};
  }
  TradingPollResult Poll(std::uint64_t,
                         model::TradingEvent&) noexcept override {
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
    config.instrument_count = 1;
    auto& instrument = config.instruments[0].instrument;
    ASSERT_EQ(model::MakeInstrumentId("SSE", "600000", instrument.id),
              ErrorCode::kOk);
    instrument.venue = model::Venue::kSse;
    std::memcpy(instrument.symbol, "600000", model::kSymbolLength);
    instrument.price_decimals = 2;
    ASSERT_TRUE(market.Configure(config).ok());
    account.cash_balance = 1000000000;
    account.position_count = 1;
    account.positions[0] = {instrument.id, 1000, 1000, 0};
    ASSERT_TRUE(ledger.Configure(account).ok());
    ASSERT_TRUE(ledger.SetMutationSink(&sink).ok());
    ledger.SetRunning(true);
  }
  model::ClientOrderId Submit(std::int64_t quantity = 100) {
    model::ClientOrderId id;
    ledger.BeginCallback(strategy::CallbackPhase::kTick);
    EXPECT_EQ(ledger.SubmitOrder({config.instruments[0].instrument.id,
                                  model::OrderSide::kBuy, 1000, quantity},
                                 id),
              ErrorCode::kOk);
    ledger.EndCallback();
    return id;
  }
  model::TradingEvent Report(model::ClientOrderId id, std::uint64_t seq,
                             model::OrderUpdateKind kind) const {
    model::TradingEvent report;
    report.client_order_id = id;
    EXPECT_EQ(report.adapter_order_id.Assign("broker-1"), ErrorCode::kOk);
    report.report_sequence = seq;
    report.kind = kind;
    report.receive_time_ns = clock.now;
    return report;
  }
  model::TradingEvent Fill(model::ClientOrderId id,
                           std::uint64_t seq = 2) const {
    auto report = Report(id, seq, model::OrderUpdateKind::kFill);
    report.cumulative_filled_qty = 30;
    report.fill.client_order_id = id;
    report.fill.adapter_order_id = report.adapter_order_id;
    EXPECT_EQ(report.fill.execution_id.Assign("fill-1"), ErrorCode::kOk);
    report.fill.instrument_id = config.instruments[0].instrument.id;
    report.fill.side = model::OrderSide::kBuy;
    report.fill.price = 999;
    report.fill.quantity = 30;
    report.fill.receive_time_ns = clock.now;
    return report;
  }
  model::OrderSnapshot Order(testing::TradingEngineHarness& trading,
                             model::ClientOrderId id) const {
    model::OrderSnapshot out;
    EXPECT_TRUE(trading.TryGetOrder(id, out));
    return out;
  }
  model::CashSnapshot Cash(testing::TradingEngineHarness& trading) const {
    model::CashSnapshot out;
    EXPECT_TRUE(trading.TryGetCash(out));
    return out;
  }
  std::vector<std::byte> Checkpoint(
      testing::TradingEngineHarness& trading) const {
    std::vector<std::byte> bytes(kMaxTradingCheckpointBytes);
    std::size_t size = 0;
    EXPECT_TRUE(trading.ExportCheckpoint(bytes, size).ok());
    bytes.resize(size);
    return bytes;
  }
  Clock clock;
  md::MarketDataConfig config;
  TradingAccountSnapshot account;
  md::InstrumentScope market;
  testing::TradingEngineHarness ledger{clock, market};
  Sink sink;
  Adapter adapter;
};

TEST_F(TradingPersistenceTest, RejectedJournalPreservesOrderAndSendQueues) {
  sink.fail = true;
  const auto cash = Cash(ledger);
  ledger.BeginCallback(strategy::CallbackPhase::kTick);
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
  EXPECT_EQ(sink.facts.back().client_order_id, id);
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
  ledger.BeginCallback(strategy::CallbackPhase::kTick);
  ASSERT_EQ(ledger.CancelOrder(id), ErrorCode::kOk);
  ledger.EndCallback();
  const auto bytes = Checkpoint(ledger);
  testing::TradingEngineHarness recovered{clock, market};
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
  testing::TradingEngineHarness replay{clock, market};
  ASSERT_TRUE(replay.Configure(account).ok());
  Sink replay_sink;
  ASSERT_TRUE(replay.SetMutationSink(&replay_sink).ok());
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
  testing::TradingEngineHarness recovered{clock, market};
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
  testing::TradingEngineHarness checkpoint_recovered{clock, market};
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
  testing::TradingEngineHarness recovered{clock, market};
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
  ledger.BeginCallback(strategy::CallbackPhase::kTick);
  EXPECT_EQ(ledger.CancelOrder(id), ErrorCode::kIoError);
  ledger.EndCallback();
  EXPECT_FALSE(Order(ledger, id).cancel_pending);
  EXPECT_EQ(Order(ledger, id).cancel_request_id.value, 0U);
  sink.fail = false;
  ledger.BeginCallback(strategy::CallbackPhase::kTick);
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
  testing::TradingEngineHarness recovered{clock, market};
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
  testing::TradingEngineHarness recovered{clock, market};
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
  testing::TradingEngineHarness again{clock, market};
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
  testing::TradingEngineHarness recovered{clock, market};
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
  testing::TradingEngineHarness checkpoint_restored{clock, market};
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
  ASSERT_EQ(terminal.adapter_order_id.Assign("broker-1"), ErrorCode::kOk);
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
  ledger.BeginCallback(strategy::CallbackPhase::kTick);
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
  ledger.BeginCallback(strategy::CallbackPhase::kTick);
  ASSERT_EQ(ledger.CancelOrder(id), ErrorCode::kOk);
  ledger.EndCallback();
  EXPECT_EQ(Order(ledger, id).cancel_request_id.value, UINT64_MAX);
  ASSERT_TRUE(ledger.ProcessRequests(adapter, clock.now).ok());
  auto rejected = Report(id, 2, model::OrderUpdateKind::kCancelRejected);
  rejected.cancel_request_id = {UINT64_MAX};
  ASSERT_EQ(ledger.ApplyEvent(rejected, dispatch), ErrorCode::kOk);
  const auto before = Checkpoint(ledger);
  ledger.BeginCallback(strategy::CallbackPhase::kTick);
  EXPECT_EQ(ledger.CancelOrder(id), ErrorCode::kResourceExhausted);
  ledger.EndCallback();
  EXPECT_EQ(Checkpoint(ledger), before);
  EXPECT_FALSE(Order(ledger, id).cancel_pending);
}

TEST_F(TradingPersistenceTest, CorruptCheckpointAndReplayPreserveLedger) {
  const auto id = Submit(10);
  auto bytes = Checkpoint(ledger);
  testing::TradingEngineHarness recovered{clock, market};
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
  fact.client_order_id = id;  // Already allocated identity must not be reused.
  EXPECT_EQ(recovered.ReplayMutation(fact).code, ErrorCode::kCorrupt);
  EXPECT_EQ(Checkpoint(recovered), before);
}
// The shared gateway persists cash accounts; the standalone futures account
// continues to trade after rejecting a persistence request.
TEST_F(TradingPersistenceTest,
       FuturesPersistenceIsRejectedBeforeAnyFinancialChange) {
  auto futures_config = config;
  auto& item = futures_config.instruments[0];
  auto& instrument = item.instrument;
  ASSERT_EQ(model::MakeInstrumentId("SHFE", "rb2701", instrument.id),
            ErrorCode::kOk);
  instrument.venue = model::Venue::kShfe;
  std::memcpy(instrument.symbol, "rb2701", model::kSymbolLength);
  instrument.type = model::InstrumentType::kFutures;
  instrument.price_decimals = 0;
  instrument.contract_multiplier = 10;
  instrument.margin_rate_ppm = 100000;
  instrument.open_fee_per_lot = 2000000;

  md::InstrumentScope futures_market;
  ASSERT_TRUE(futures_market.Configure(futures_config).ok());
  TradingAccountSnapshot initial;
  initial.type = model::AccountType::kFutures;
  initial.cash_balance = 1000000000;
  testing::TradingEngineHarness futures(clock, futures_market);
  ASSERT_TRUE(futures.Configure(initial).ok());
  model::FuturesAccountSnapshot before;
  ASSERT_TRUE(futures.TryGetFuturesAccount(before));
  Sink rejected_sink;
  rejected_sink.fail = true;
  EXPECT_EQ(futures.SetMutationSink(&rejected_sink).code,
            ErrorCode::kUnsupported);
  std::array<std::byte, 128> output{};
  const auto original = output;
  std::size_t size = 77;
  EXPECT_EQ(futures.ExportCheckpoint(output, size).code,
            ErrorCode::kUnsupported);
  EXPECT_EQ(size, 77U);
  EXPECT_EQ(output, original);
  EXPECT_EQ(futures.RestoreCheckpoint(Checkpoint(ledger)).code,
            ErrorCode::kUnsupported);
  EXPECT_EQ(futures.RestoreReconciled(account, {}, {}).code,
            ErrorCode::kUnsupported);
  TradingMutation mutation;
  mutation.revision = 1;
  EXPECT_EQ(futures.ReplayMutation(mutation).code, ErrorCode::kUnsupported);
  model::FuturesAccountSnapshot after;
  ASSERT_TRUE(futures.TryGetFuturesAccount(after));
  EXPECT_EQ(after, before);

  futures.SetRunning(true);
  futures.BeginCallback(strategy::CallbackPhase::kTimer);
  model::ClientOrderId id;
  ASSERT_EQ(futures.SubmitOrder({instrument.id, model::OrderSide::kBuy, 100, 1,
                                 model::PositionEffect::kOpen},
                                id),
            ErrorCode::kOk);
  futures.EndCallback();
  ASSERT_TRUE(futures.TryGetFuturesAccount(after));
  EXPECT_EQ(after.reserved, 102000000);
  EXPECT_EQ(Order(futures, id).position_effect, model::PositionEffect::kOpen);
  futures.BeginStopping();
  ASSERT_TRUE(futures.ProcessRequests(adapter, clock.now).ok());
  TradingDispatch dispatch;
  ASSERT_EQ(futures.ApplyLocalUpdate(dispatch), ErrorCode::kOk);
  EXPECT_EQ(dispatch.update.order.position_effect,
            model::PositionEffect::kOpen);
  ASSERT_TRUE(futures.TryGetFuturesAccount(after));
  EXPECT_EQ(after, before);
  EXPECT_EQ(futures.mutation_revision(), 0U);
  EXPECT_TRUE(rejected_sink.facts.empty());
  EXPECT_EQ(adapter.sends, 0U);
  EXPECT_TRUE(futures.SetMutationSink(nullptr).ok());
}

TEST_F(TradingPersistenceTest, RejectsFuturesBaselineOnCashLedger) {
  ledger.SetRunning(false);
  const auto before = Checkpoint(ledger);
  auto baseline = account;
  baseline.type = model::AccountType::kFutures;
  EXPECT_EQ(ledger.RestoreReconciled(baseline, {}, {}).code,
            ErrorCode::kUnsupported);
  EXPECT_EQ(Checkpoint(ledger), before);
  baseline.type = model::AccountType::kCash;
  baseline.futures_position_count = 1;
  EXPECT_FALSE(ledger.RestoreReconciled(baseline, {}, {}).ok());
  EXPECT_EQ(Checkpoint(ledger), before);
}

TEST_F(TradingPersistenceTest, CashMutationCodecRejectsFuturesEffect) {
  TradingMutation mutation;
  mutation.kind = TradingMutationKind::kNewOrder;
  mutation.revision = 1;
  mutation.client_order_id = {1};
  mutation.request = {config.instruments[0].instrument.id,
                      model::OrderSide::kBuy, 1000, 10,
                      model::PositionEffect::kOpen};
  std::array<std::byte, kMaxEncodedTradingMutation> bytes{};
  std::size_t size = 77;
  EXPECT_EQ(EncodeTradingMutation(mutation, bytes, size).code,
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(size, 77U);
  mutation = {};
  mutation.kind = TradingMutationKind::kReport;
  mutation.revision = 1;
  mutation.client_order_id = {1};
  mutation.event = Fill({1});
  mutation.event.fill.position_effect = model::PositionEffect::kOpen;
  EXPECT_EQ(EncodeTradingMutation(mutation, bytes, size).code,
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(size, 77U);
}

TEST_F(TradingPersistenceTest, SinkCannotDetachFromItsMutationCallback) {
  class DetachingSink final : public TradingMutationSink {
   public:
    explicit DetachingSink(testing::TradingEngineHarness& owner)
        : owner_(owner) {}
    base::Status BeforeCommit(const TradingMutation&) noexcept override {
      ++calls;
      detach = owner_.SetMutationSink(nullptr);
      return {};
    }
    base::Status detach;
    std::size_t calls = 0;

   private:
    testing::TradingEngineHarness& owner_;
  } detaching(ledger);
  ASSERT_TRUE(ledger.SetMutationSink(&detaching).ok());
  Submit(10);
  EXPECT_EQ(detaching.detach.code, ErrorCode::kInvalidState);
  Submit(10);
  EXPECT_EQ(detaching.calls, 2U);
  EXPECT_EQ(detaching.detach.code, ErrorCode::kInvalidState);
  ASSERT_TRUE(ledger.SetMutationSink(nullptr).ok());
}

TEST_F(TradingPersistenceTest,
       ReplacingSinkDuringCallbackPreservesTheInstalledSink) {
  Sink replacement;
  ledger.BeginCallback(strategy::CallbackPhase::kTimer);
  EXPECT_EQ(ledger.SetMutationSink(&replacement).code,
            ErrorCode::kInvalidState);
  EXPECT_EQ(ledger.SetMutationSink(nullptr).code, ErrorCode::kInvalidState);
  ledger.EndCallback();
  Submit(10);
  EXPECT_EQ(sink.facts.size(), 1U);
  EXPECT_TRUE(replacement.facts.empty());
}
}  // namespace
}  // namespace yquant::trading
