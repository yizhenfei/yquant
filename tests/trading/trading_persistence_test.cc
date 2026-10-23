#include "yquant/trading/trading_persistence.h"

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "gtest/gtest.h"
#include "trading_engine_harness.h"
#include "yquant/trading/trading_engine.h"

namespace yquant::trading {
namespace {
using base::ErrorCode;

class Clock final : public base::MonotonicClock {
 public:
  uint64_t Now() const noexcept override { return now; }
  uint64_t now = 1000;
};

class Adapter final : public TradingAdapter {
 public:
  base::Status Prepare(const TradingConfig&) noexcept override { return {}; }
  base::Status Start(uint64_t) noexcept override { return {}; }
  base::Status ReadAccountSnapshot(TradingAccountSnapshot&) noexcept override {
    return {};
  }
  base::Status SendOrder(model::ClientOrderId, const model::OrderRequest&,
                         uint64_t) noexcept override {
    ++sends;
    return {};
  }
  base::Status SendCancel(model::ClientOrderId, const model::AdapterOrderId&,
                          model::CancelRequestId, uint64_t) noexcept override {
    ++sends;
    return {};
  }
  TradingPollResult Poll(uint64_t, model::TradingEvent&) noexcept override {
    return {};
  }
  std::optional<uint64_t> next_deadline() const noexcept override { return {}; }
  bool is_drained() const noexcept override { return true; }
  base::Status Stop() noexcept override { return {}; }
  const char* error_detail() const noexcept override { return ""; }
  size_t sends = 0;
};

// Captures copies so tests can replay after the original callback returns.
class Sink final : public TradingMutationSink {
 public:
  base::Status BeforeCommit(const TradingMutation& fact) noexcept override {
    if (fail) {
      return {ErrorCode::kIoError, {}};
    }
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
  model::ClientOrderId Submit(int64_t quantity = 100) {
    model::ClientOrderId id;
    ledger.BeginCallback(strategy::CallbackPhase::kTick);
    EXPECT_EQ(ledger.SubmitOrder({config.instruments[0].instrument.id,
                                  model::OrderSide::kBuy, 1000, quantity},
                                 id),
              ErrorCode::kOk);
    ledger.EndCallback();
    return id;
  }
  model::TradingEvent Report(model::ClientOrderId id, uint64_t seq,
                             model::OrderUpdateKind kind) const {
    model::TradingEvent report;
    report.client_order_id = id;
    EXPECT_EQ(report.adapter_order_id.Assign("broker-1"), ErrorCode::kOk);
    report.report_sequence = seq;
    report.kind = kind;
    report.receive_time_ns = clock.now;
    return report;
  }
  model::TradingEvent Fill(model::ClientOrderId id, uint64_t seq = 2) const {
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
    size_t size = 0;
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
    size_t size = 0;
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
       QueuedCancelLocalResultSurvivesCheckpointAndRejectedJournal) {
  const auto id = Submit(30);
  ASSERT_TRUE(ledger.ProcessRequests(adapter, clock.now).ok());
  TradingDispatch dispatch;
  ASSERT_EQ(ledger.ApplyEvent(Report(id, 1, model::OrderUpdateKind::kAccepted),
                              dispatch),
            ErrorCode::kOk);
  ledger.BeginCallback(strategy::CallbackPhase::kTick);
  ASSERT_EQ(ledger.CancelOrder(id), ErrorCode::kOk);
  ledger.EndCallback();
  ++clock.now;
  ASSERT_EQ(ledger.ApplyEvent(Fill(id), dispatch), ErrorCode::kOk);
  const auto filled = Order(ledger, id);
  const auto cash = Cash(ledger);
  EXPECT_EQ(filled.state, model::OrderState::kFilled);
  EXPECT_TRUE(filled.cancel_pending);
  EXPECT_EQ(filled.report_sequence, 2U);
  EXPECT_EQ(filled.receive_time_ns, 1001U);
  EXPECT_EQ(cash.balance, 700300000);
  EXPECT_EQ(cash.reserved, 0);
  ASSERT_TRUE(ledger.has_local_updates());
  const auto pending = Checkpoint(ledger);
  testing::TradingEngineHarness recovered{clock, market};
  ASSERT_TRUE(recovered.RestoreCheckpoint(pending).ok());
  EXPECT_EQ(Checkpoint(recovered), pending);
  EXPECT_TRUE(recovered.has_local_updates());
  Sink recovered_sink;
  ASSERT_TRUE(recovered.SetMutationSink(&recovered_sink).ok());
  recovered_sink.fail = true;
  dispatch.notify_order = true;
  dispatch.has_fill = true;
  dispatch.update.order.client_order_id = {999};
  dispatch.fill.quantity = 777;
  EXPECT_EQ(recovered.ApplyLocalUpdate(dispatch), ErrorCode::kIoError);
  EXPECT_EQ(Checkpoint(recovered), pending);
  EXPECT_TRUE(recovered.has_local_updates());
  EXPECT_TRUE(dispatch.notify_order);
  EXPECT_TRUE(dispatch.has_fill);
  EXPECT_EQ(dispatch.update.order.client_order_id.value, 999U);
  EXPECT_EQ(dispatch.fill.quantity, 777);
  EXPECT_TRUE(recovered_sink.facts.empty());

  recovered_sink.fail = false;
  clock.now += 100;
  ASSERT_EQ(recovered.ApplyLocalUpdate(dispatch), ErrorCode::kOk);
  EXPECT_TRUE(dispatch.notify_order);
  EXPECT_FALSE(dispatch.has_fill);
  EXPECT_EQ(dispatch.update.kind, model::OrderUpdateKind::kCancelNotSent);
  EXPECT_EQ(dispatch.update.cancel_request_id, filled.cancel_request_id);
  auto settled = filled;
  settled.cancel_pending = false;
  EXPECT_EQ(Order(recovered, id), settled);
  EXPECT_EQ(dispatch.update.order, settled);
  EXPECT_EQ(Cash(recovered), cash);
  EXPECT_EQ(recovered.fill_count(), 1U);
  EXPECT_FALSE(recovered.has_local_updates());
  EXPECT_EQ(recovered.mutation_revision(), ledger.mutation_revision() + 1);
  ASSERT_EQ(recovered_sink.facts.size(), 1U);
  EXPECT_EQ(recovered_sink.facts[0].kind, TradingMutationKind::kCancelNotSent);
  EXPECT_EQ(recovered_sink.facts[0].cancel_id, filled.cancel_request_id);
  ASSERT_TRUE(recovered.ProcessRequests(adapter, clock.now).ok());
  EXPECT_EQ(adapter.sends, 1U);
}

TEST_F(TradingPersistenceTest,
       EveryLocalCancelResultCutMatchesContinuousCheckpoint) {
  const auto id = Submit(30);
  ASSERT_TRUE(ledger.ProcessRequests(adapter, clock.now).ok());
  TradingDispatch dispatch;
  ASSERT_EQ(ledger.ApplyEvent(Report(id, 1, model::OrderUpdateKind::kAccepted),
                              dispatch),
            ErrorCode::kOk);
  ledger.BeginCallback(strategy::CallbackPhase::kTick);
  ASSERT_EQ(ledger.CancelOrder(id), ErrorCode::kOk);
  ledger.EndCallback();
  ++clock.now;
  ASSERT_EQ(ledger.ApplyEvent(Fill(id), dispatch), ErrorCode::kOk);
  ++clock.now;
  ASSERT_EQ(ledger.ApplyLocalUpdate(dispatch), ErrorCode::kOk);
  const auto expected = Checkpoint(ledger);
  ASSERT_EQ(sink.facts.size(), 6U);
  EXPECT_EQ(sink.facts[4].kind, TradingMutationKind::kReport);
  EXPECT_EQ(sink.facts[5].kind, TradingMutationKind::kCancelNotSent);
  clock.now = 1;
  for (size_t cut = 0; cut <= sink.facts.size(); ++cut) {
    SCOPED_TRACE(cut);
    testing::TradingEngineHarness prefix{clock, market};
    ASSERT_TRUE(prefix.Configure(account).ok());
    for (size_t i = 0; i < cut; ++i) {
      ASSERT_TRUE(prefix.ReplayMutation(sink.facts[i]).ok())
          << prefix.error_detail();
    }
    const auto checkpoint = Checkpoint(prefix);
    testing::TradingEngineHarness resumed{clock, market};
    ASSERT_TRUE(resumed.RestoreCheckpoint(checkpoint).ok())
        << resumed.error_detail();
    Sink replay_sink;
    ASSERT_TRUE(resumed.SetMutationSink(&replay_sink).ok());
    for (size_t i = cut; i < sink.facts.size(); ++i) {
      std::array<std::byte, kMaxEncodedTradingMutation> bytes;
      size_t size = 0;
      ASSERT_TRUE(EncodeTradingMutation(sink.facts[i], bytes, size).ok());
      TradingMutation decoded;
      ASSERT_TRUE(
          DecodeTradingMutation(std::span(bytes).first(size), decoded).ok());
      ASSERT_TRUE(resumed.ReplayMutation(decoded, &dispatch).ok())
          << resumed.error_detail();
    }
    EXPECT_EQ(Checkpoint(resumed), expected);
    EXPECT_EQ(Order(resumed, id), Order(ledger, id));
    EXPECT_EQ(Cash(resumed), Cash(ledger));
    EXPECT_FALSE(resumed.has_local_updates());
    EXPECT_TRUE(replay_sink.facts.empty());
  }
  EXPECT_EQ(adapter.sends, 1U);
}

TEST_F(TradingPersistenceTest,
       LegacyFactsKeepQueuedCancelUntilHistoricalSendAndResponse) {
  const model::ClientOrderId id{1};
  const model::CancelRequestId cancel_id{1};
  std::array<TradingMutation, 7> facts;
  for (size_t i = 0; i < facts.size(); ++i) {
    facts[i].format_version = 1;
    facts[i].revision = i + 1;
    facts[i].now = clock.now;
    facts[i].client_order_id = id;
  }
  facts[0].kind = TradingMutationKind::kNewOrder;
  facts[0].request = {config.instruments[0].instrument.id,
                      model::OrderSide::kBuy, 1000, 30};
  facts[1].kind = TradingMutationKind::kSendNew;
  facts[2].kind = TradingMutationKind::kReport;
  facts[2].event = Report(id, 1, model::OrderUpdateKind::kAccepted);
  facts[3].kind = TradingMutationKind::kCancel;
  facts[3].cancel_id = cancel_id;
  facts[4].kind = TradingMutationKind::kReport;
  facts[4].event = Fill(id);
  facts[5].kind = TradingMutationKind::kSendCancel;
  facts[5].cancel_id = cancel_id;
  facts[6].kind = TradingMutationKind::kReport;
  facts[6].event = Report(id, 3, model::OrderUpdateKind::kCancelRejected);
  facts[6].event.cancel_request_id = cancel_id;
  facts[6].event.cumulative_filled_qty = 30;
  testing::TradingEngineHarness replay{clock, market};
  ASSERT_TRUE(replay.Configure(account).ok());
  TradingDispatch dispatch;
  for (size_t i = 0; i < facts.size(); ++i) {
    SCOPED_TRACE(i);
    std::array<std::byte, kMaxEncodedTradingMutation> bytes;
    size_t size = 0;
    ASSERT_TRUE(EncodeTradingMutation(facts[i], bytes, size).ok());
    TradingMutation decoded;
    ASSERT_TRUE(
        DecodeTradingMutation(std::span(bytes).first(size), decoded).ok());
    EXPECT_EQ(decoded.format_version, 1);
    ASSERT_TRUE(replay.ReplayMutation(decoded, &dispatch).ok())
        << replay.error_detail();
    if (i == 4 || i == 5) {
      EXPECT_EQ(Order(replay, id).state, model::OrderState::kFilled);
      EXPECT_TRUE(Order(replay, id).cancel_pending);
      EXPECT_FALSE(replay.has_local_updates());
    }
  }
  const auto settled = Order(replay, id);
  EXPECT_EQ(settled.state, model::OrderState::kFilled);
  EXPECT_FALSE(settled.cancel_pending);
  EXPECT_EQ(settled.report_sequence, 3U);
  EXPECT_EQ(settled.cancel_request_id, cancel_id);
  EXPECT_EQ(dispatch.update.kind, model::OrderUpdateKind::kCancelRejected);
  EXPECT_FALSE(dispatch.has_fill);
  EXPECT_EQ(Cash(replay).balance, 700300000);
  EXPECT_EQ(Cash(replay).reserved, 0);
  EXPECT_EQ(replay.fill_count(), 1U);
  EXPECT_EQ(replay.mutation_revision(), 7U);
  EXPECT_EQ(adapter.sends, 0U);
  testing::TradingEngineHarness restored{clock, market};
  ASSERT_TRUE(restored.RestoreCheckpoint(Checkpoint(replay)).ok())
      << restored.error_detail();
  EXPECT_EQ(Order(restored, id), settled);
  EXPECT_EQ(Checkpoint(restored), Checkpoint(replay));
}

TEST_F(TradingPersistenceTest, LegacyTerminalWithoutBrokerSequenceRestores) {
  std::vector<std::byte> bytes;
  auto append = [&](uint64_t value, size_t width = 8) {
    for (size_t i = 0; i < width; ++i) {
      bytes.push_back(static_cast<std::byte>(value & 0xffU));
      value >>= 8;
    }
  };
  // Version 1 reconciled baselines could contain terminal orders with no
  // normalized broker sequence, while recording the new request as attempted.
  append(0x3154504344515955ULL);  // Cash-checkpoint magic.
  append(1, 4);                   // Format version.
  append(0);                      // Mutation revision.
  append(2);                      // Next order ID.
  append(0);                      // Last cancel ID.
  append(1000000000);             // Cash balance.
  append(0);                      // Reserved cash.
  append(1000000000);             // Available cash.
  append(1, 4);                   // Position count.
  append(config.instruments[0].instrument.id.value);
  append(1000);  // Total shares.
  append(1000);  // Available shares.
  append(0);     // Reserved shares.
  append(1, 4);  // Order count.
  append(0, 4);  // Fill count.
  append(0, 4);  // Active count.
  append(0, 4);  // Pending cancel count.
  append(0, 4);  // Stopping scan cursor.
  append(0, 1);  // Scan pending.
  append(0, 1);  // Rescan required.
  append(0, 1);  // No retry deadline.
  append(0);
  append(1);     // ClientOrderId.
  append(0, 1);  // Empty AdapterOrderId.
  append(config.instruments[0].instrument.id.value);
  append(1, 1);   // Buy.
  append(1000);   // Limit price.
  append(30);     // Original quantity.
  append(0);      // Cumulative filled quantity.
  append(0);      // Leaves quantity.
  append(3, 1);   // Canceled.
  append(0, 1);   // No pending cancel.
  append(0);      // CancelRequestId.
  append(0);      // Broker report sequence.
  append(0);      // Broker receive time.
  append(1, 1);   // New request attempted.
  append(0, 1);   // Cancel not attempted.
  append(0, 1);   // No pending local result.
  append(0);      // Per-order retry deadline.
  append(0, 4);   // Position index.
  append(10000);  // Price-to-micro-yuan multiplier.
  append(0);      // Per-order reserved cash.
  append(0);      // Per-order reserved shares.
  append(0, 4);   // Queued requests.
  append(0, 4);   // Queued local results.
  testing::TradingEngineHarness recovered{clock, market};
  ASSERT_TRUE(recovered.RestoreCheckpoint(bytes).ok())
      << recovered.error_detail();
  const auto order = Order(recovered, {1});
  EXPECT_EQ(order.state, model::OrderState::kCanceled);
  EXPECT_EQ(order.report_sequence, 0U);
  EXPECT_EQ(order.receive_time_ns, 0U);
  EXPECT_EQ(order.original_qty, 30);
  EXPECT_EQ(Cash(recovered), Cash(ledger));
  EXPECT_EQ(recovered.next_order_id(), 2U);
  EXPECT_FALSE(recovered.has_local_updates());
  ASSERT_TRUE(recovered.ProcessRequests(adapter, clock.now).ok());
  EXPECT_EQ(adapter.sends, 0U);
  testing::TradingEngineHarness again{clock, market};
  ASSERT_TRUE(again.RestoreCheckpoint(Checkpoint(recovered)).ok())
      << again.error_detail();
  EXPECT_EQ(Order(again, {1}), order);
}

TEST_F(TradingPersistenceTest, ChangedImmutableCoreRulesRejectCheckpoint) {
  Submit(30);
  const auto bytes = Checkpoint(ledger);
  for (const bool change_price_tick : {true, false}) {
    SCOPED_TRACE(change_price_tick ? "price tick" : "quantity step");
    auto changed = config;
    auto& instrument = changed.instruments[0].instrument;
    if (change_price_tick) {
      instrument.price_tick = 5;
    } else {
      instrument.quantity_step = 2;
    }
    md::InstrumentScope changed_market;
    ASSERT_TRUE(changed_market.Configure(changed).ok());
    testing::TradingEngineHarness recovered{clock, changed_market};
    ASSERT_TRUE(recovered.Configure(account).ok());
    const auto before = Checkpoint(recovered);
    EXPECT_EQ(recovered.RestoreCheckpoint(bytes).code, ErrorCode::kCorrupt);
    EXPECT_EQ(Checkpoint(recovered), before);
    EXPECT_EQ(recovered.order_count(), 0U);
  }
}

TEST_F(TradingPersistenceTest,
       InvalidPublicReplayFactsPreserveCheckpointAndDispatch) {
  const auto id = Submit(30);
  ASSERT_TRUE(ledger.ProcessRequests(adapter, clock.now).ok());
  TradingMutation fault;
  fault.revision = ledger.mutation_revision() + 1;
  fault.now = clock.now;
  fault.client_order_id = id;
  fault.kind = TradingMutationKind::kRequestUncertain;
  fault.failure_code = ErrorCode::kIoError;
  std::array<TradingMutation, 4> invalid = {fault, fault, fault, fault};
  invalid[0].format_version = 1;
  invalid[0].kind = TradingMutationKind::kCancelNotSent;
  invalid[0].cancel_id = {1};
  invalid[1].format_version = 1;
  invalid[2].failure_code = ErrorCode::kOk;
  invalid[3].failure_code = static_cast<ErrorCode>(255);
  const auto before = Checkpoint(ledger);
  const auto fact_count = sink.facts.size();
  TradingDispatch dispatch;
  dispatch.notify_order = true;
  dispatch.has_fill = true;
  dispatch.update.order.client_order_id = {999};
  dispatch.update.reason.Set(91, "preserved on rejection");
  dispatch.fill.quantity = 777;
  for (const auto& fact : invalid) {
    SCOPED_TRACE(static_cast<int>(fact.kind));
    SCOPED_TRACE(fact.format_version);
    SCOPED_TRACE(static_cast<int>(fact.failure_code));
    EXPECT_EQ(ledger.ReplayMutation(fact, &dispatch).code, ErrorCode::kCorrupt);
    EXPECT_EQ(Checkpoint(ledger), before);
    EXPECT_EQ(sink.facts.size(), fact_count);
    EXPECT_TRUE(dispatch.notify_order);
    EXPECT_TRUE(dispatch.has_fill);
    EXPECT_EQ(dispatch.update.order.client_order_id.value, 999U);
    EXPECT_EQ(dispatch.update.reason.code, 91);
    EXPECT_EQ(dispatch.update.reason.view(), "preserved on rejection");
    EXPECT_EQ(dispatch.fill.quantity, 777);
  }
}

TEST_F(TradingPersistenceTest, LegacySendAtFinalClockValueHasNoNewDeadline) {
  const model::ClientOrderId id{1};
  std::array<TradingMutation, 3> facts;
  for (size_t i = 0; i < facts.size(); ++i) {
    facts[i].format_version = 1;
    facts[i].revision = i + 1;
    facts[i].now = UINT64_MAX;
    facts[i].client_order_id = id;
  }
  facts[0].kind = TradingMutationKind::kNewOrder;
  facts[0].request = {config.instruments[0].instrument.id,
                      model::OrderSide::kBuy, 1000, 30};
  facts[1].kind = TradingMutationKind::kSendNew;
  facts[2].kind = TradingMutationKind::kReport;
  facts[2].event = Report(id, 1, model::OrderUpdateKind::kAccepted);
  testing::TradingEngineHarness replay{clock, market};
  ASSERT_TRUE(replay.Configure(account).ok());
  for (size_t i = 0; i < facts.size(); ++i) {
    SCOPED_TRACE(i);
    std::array<std::byte, kMaxEncodedTradingMutation> bytes;
    size_t size = 0;
    ASSERT_TRUE(EncodeTradingMutation(facts[i], bytes, size).ok());
    TradingMutation decoded;
    ASSERT_TRUE(
        DecodeTradingMutation(std::span(bytes).first(size), decoded).ok());
    ASSERT_TRUE(replay.ReplayMutation(decoded).ok()) << replay.error_detail();
    if (i == 1) {
      EXPECT_EQ(Order(replay, id).state, model::OrderState::kPendingNew);
      EXPECT_FALSE(replay.next_deadline());
      testing::TradingEngineHarness restored{clock, market};
      ASSERT_TRUE(restored.RestoreCheckpoint(Checkpoint(replay)).ok())
          << restored.error_detail();
      EXPECT_EQ(Checkpoint(restored), Checkpoint(replay));
    }
  }
  EXPECT_EQ(Order(replay, id).state, model::OrderState::kWorking);
  EXPECT_EQ(Order(replay, id).report_sequence, 1U);
  EXPECT_EQ(adapter.sends, 0U);
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
  for (const auto& fact : sink.facts) {
    ASSERT_TRUE(recovered.ReplayMutation(fact, &dispatch).ok());
  }
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
  for (const auto& fact : sink.facts) {
    ASSERT_TRUE(recovered.ReplayMutation(fact).ok());
  }
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
  for (size_t size = 0; size < before.size(); ++size) {
    EXPECT_EQ(recovered.RestoreCheckpoint(std::span(before).first(size)).code,
              ErrorCode::kCorrupt);
  }
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
  size_t size = 77;
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
  size_t size = 77;
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
    size_t calls = 0;

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
