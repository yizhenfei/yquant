#include "tradingd/archive_projection.h"

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "gtest/gtest.h"
#include "market/pipeline_test_support.h"
#include "trading_engine_harness.h"
#include "tradingd/state_codec.h"
#include "yquant/base/format.h"
#include "yquant/trading/mock_trading_adapter.h"

namespace yquant::tradingd {
namespace {
using base::ErrorCode;

class ArchiveClock final : public base::MonotonicClock {
 public:
  uint64_t Now() const noexcept override { return now; }
  uint64_t now = 1000;
};

// Captures the same owned facts that gateway WAL carries.
class FactSink final : public trading::TradingMutationSink {
 public:
  base::Status BeforeCommit(
      const trading::TradingMutation& mutation) noexcept override {
    facts.push_back(mutation);
    return {};
  }
  std::vector<trading::TradingMutation> facts;
};

class ArchiveProjectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config.market_data = market::testing::ScopeConfig();
    config.trading.initial_account.cash_balance = 1000000000;
    ::yquant::base::FormatTo(config.persistence.directory,
                             sizeof(config.persistence.directory), "/unused");
    ::yquant::base::FormatTo(config.persistence.account_key,
                             sizeof(config.persistence.account_key),
                             "mock/account");
    ASSERT_TRUE(market.Configure(config.market_data).ok());
    ASSERT_TRUE(source.Configure(config.trading.initial_account).ok());
    ASSERT_TRUE(source.SetMutationSink(&sink).ok());
    source.SetRunning(true);
    ASSERT_TRUE(projection.Configure(config).ok());
  }

  // Mirrors identity v2's explicit fields, independently of projection code.
  std::vector<std::byte> Identity(bool second = false,
                                  uint64_t first_client = 41) {
    std::vector<std::byte> bytes(32768);
    StateWriter out(bytes);
    out.U64(2);
    out.Text(config.persistence.account_key);
    out.U64(11);
    out.U64(12);
    out.U64(second ? 4 : 3);
    out.U64(1024);
    out.U64(8192);
    out.U64(config.market_data.instrument_count);
    for (size_t i = 0; i < config.market_data.instrument_count; ++i) {
      const auto& d = config.market_data.instruments[i].instrument;
      out.U64(d.id.value);
      out.U64(static_cast<unsigned>(d.venue));
      out.Text(d.symbol_view());
      out.U64(d.price_decimals);
      out.U64(static_cast<unsigned>(d.type));
      out.U64(d.price_tick);
      out.U64(d.quantity_step);
      out.U64(d.contract_multiplier);
      out.U64(d.margin_rate_ppm);
      out.U64(d.open_fee_per_lot);
      out.U64(d.close_today_fee_per_lot);
      out.U64(d.close_yesterday_fee_per_lot);
    }
    out.U64(static_cast<unsigned>(config.trading.mode));
    out.U64(config.trading.report_interval_ns);
    for (size_t i = 0; i < trading::kMaxClients; ++i) {
      const bool present = i == 0 || (second && i == 1);
      out.U64(present ? (i == 0 ? first_client : 42) : 0);
      out.U64(present ? (i == 0 ? 2 : 4) : 0);
      out.U64(present ? 1 : 0);
      if (present) {
        out.U64(config.market_data.instruments[0].instrument.id.value);
        out.U64(config.market_data.instruments[0].instrument.price_decimals);
      }
    }
    EXPECT_TRUE(out.ok());
    bytes.resize(out.size());
    return bytes;
  }

  std::vector<std::byte> Baseline(bool has_order = false) {
    std::vector<std::byte> checkpoint(8 * 1024 * 1024);
    StateWriter out(checkpoint);
    out.U64(1);
    out.U64(11);
    out.U64(12);
    out.U64(1);
    out.U64(0);
    out.U64(1);
    out.U64(64);
    out.U64(clock.Now());
    for (size_t i = 0; i < trading::kMaxClients; ++i) {
      out.U64(i == 0 ? 41 : 0);
      out.U64(i == 0 ? 2 : 0);
      out.U64(i == 0 && has_order ? 1 : 0);
      out.U64(0);
      if (i == 0 && has_order) {
        out.U64(1);
        out.U64(2);
        out.U64(0);
        out.U64(0);
        out.U64(0);
      }
    }
    std::vector<std::byte> ledger(trading::kMaxTradingCheckpointBytes);
    size_t size = 0;
    EXPECT_TRUE(source.ExportCheckpoint(ledger, size).ok());
    out.U64(size);
    out.Bytes(std::span(ledger).first(size));
    out.U64(0);  // Mock image is irrelevant to archive accounting.
    EXPECT_TRUE(out.ok());
    checkpoint.resize(out.size());
    const auto identity = Identity();
    std::vector<std::byte> bytes(checkpoint.size() + identity.size() + 256);
    StateWriter baseline(bytes);
    baseline.U64(1);
    baseline.U64(11);
    baseline.U64(12);
    baseline.U64(1);
    baseline.Text(config.persistence.account_key);
    baseline.U64(identity.size());
    baseline.Bytes(identity);
    baseline.U64(checkpoint.size());
    baseline.Bytes(checkpoint);
    EXPECT_TRUE(baseline.ok());
    bytes.resize(baseline.size());
    return bytes;
  }

  void SubmitAndFill() {
    source.BeginCallback(strategy::CallbackPhase::kTick);
    model::ClientOrderId id;
    ASSERT_EQ(
        source.SubmitOrder({config.market_data.instruments[0].instrument.id,
                            model::OrderSide::kBuy, 1000, 10},
                           id),
        ErrorCode::kOk);
    source.EndCallback();
    ASSERT_TRUE(mock.Prepare(config.trading).ok());
    ASSERT_TRUE(mock.Start(clock.Now()).ok());
    ASSERT_TRUE(source.ProcessRequests(mock, clock.Now()).ok());
    model::TradingEvent accepted;
    clock.now += config.trading.report_interval_ns;
    ASSERT_EQ(mock.Poll(clock.Now(), accepted).state,
              trading::TradingPollState::kEvent);
    trading::TradingDispatch dispatch;
    ASSERT_EQ(source.ApplyEvent(accepted, dispatch), ErrorCode::kOk);
    auto fill = accepted;
    fill.report_sequence = 2;
    fill.kind = model::OrderUpdateKind::kFill;
    fill.cumulative_filled_qty = 4;
    fill.fill.client_order_id = id;
    fill.fill.adapter_order_id = accepted.adapter_order_id;
    ASSERT_EQ(fill.fill.execution_id.Assign("execution-1"), ErrorCode::kOk);
    fill.fill.instrument_id = config.market_data.instruments[0].instrument.id;
    fill.fill.side = model::OrderSide::kBuy;
    fill.fill.price = 999;
    fill.fill.quantity = 4;
    fill.fill.receive_time_ns = clock.Now();
    ASSERT_EQ(source.ApplyEvent(fill, dispatch), ErrorCode::kOk);
  }

  std::vector<std::byte> LedgerRecord(const trading::TradingMutation& mutation,
                                      size_t slot = 0, size_t local = 0,
                                      uint64_t cancel = 0) {
    std::vector<std::byte> bytes(2048);
    StateWriter writer(bytes);
    writer.U64(slot);
    writer.U64(local);
    writer.U64(cancel);
    size_t size = 0;
    EXPECT_TRUE(
        trading::EncodeTradingMutation(mutation, writer.tail(), size).ok());
    writer.Advance(size);
    bytes.resize(writer.size());
    return bytes;
  }

  std::vector<std::byte> RejectionRecord(const trading::Message& message,
                                         size_t local = 0) {
    std::vector<std::byte> bytes(8192);
    StateWriter writer(bytes);
    writer.U64(0);
    writer.U64(local);
    std::array<std::byte, trading::kMaxFrameSize> encoded{};
    size_t size = 0;
    EXPECT_TRUE(trading::EncodeMessage(message, encoded, size).ok());
    writer.U64(size);
    writer.Bytes(std::span(encoded).first(size));
    bytes.resize(writer.size());
    return bytes;
  }

  ArchiveClock clock;
  ServerConfig config;
  market::InstrumentScope market;
  testing::TradingEngineHarness source{clock, market};
  trading::MockTradingAdapter mock;
  FactSink sink;
  ArchiveProjection projection{&clock};
};

TEST_F(ArchiveProjectionTest, ReplaysAccountingOnceAndRetainsSourceIdentity) {
  ASSERT_TRUE(projection.Restore(Baseline()).ok());
  SubmitAndFill();
  uint64_t lsn = 0;
  for (const auto& fact : sink.facts) {
    ASSERT_TRUE(projection.Replay(1, ++lsn, LedgerRecord(fact)).ok());
  }
  ASSERT_EQ(projection.orders().size(), 1);
  const auto& order = projection.orders()[0];
  EXPECT_EQ(order.client_id, 41);
  EXPECT_EQ(order.session_id, 2);
  EXPECT_EQ(order.local_order_id, 1);
  EXPECT_EQ(order.snapshot.cumulative_filled_qty, 4);
  EXPECT_EQ(order.report_sequence, 2);
  EXPECT_EQ(order.source_lsn, lsn);
  model::CashSnapshot expected, actual;
  ASSERT_TRUE(source.TryGetCash(expected));
  ASSERT_TRUE(projection.trading().TryGetCash(actual));
  EXPECT_EQ(actual, expected);
  ASSERT_EQ(projection.trading().fill_count(), 1);
  EXPECT_EQ(projection.FillSourceLsn(0), lsn);
  ASSERT_TRUE(projection.has_update());
  EXPECT_TRUE(projection.last_update().has_fill);
  EXPECT_EQ(projection.last_update().fill.strategy_order_id.value, 1);

  trading::MockTradingMutation mutation;
  mutation.client_order_id = {1};
  mutation.request = sink.facts.front().request;
  mutation.now = 2000;
  std::array<std::byte, trading::kMaxMockTradingMutationBytes> bytes{};
  size_t size = 0;
  ASSERT_TRUE(trading::EncodeMockTradingMutation(mutation, bytes, size).ok());
  ASSERT_TRUE(projection.Replay(2, ++lsn, std::span(bytes).first(size)).ok());
  EXPECT_FALSE(projection.has_update());
  EXPECT_EQ(projection.trading().fill_count(), 1);
  ASSERT_TRUE(projection.trading().TryGetCash(actual));
  EXPECT_EQ(actual, expected);
}

TEST_F(ArchiveProjectionTest, BaselineRestoresHistoricalFillsWithoutDebiting) {
  SubmitAndFill();
  ASSERT_TRUE(projection.Restore(Baseline(true)).ok());
  ASSERT_EQ(projection.orders().size(), 1);
  EXPECT_EQ(projection.orders()[0].snapshot.cumulative_filled_qty, 4);
  EXPECT_EQ(projection.orders()[0].source_lsn, 0);
  ASSERT_EQ(projection.trading().fill_count(), 1);
  EXPECT_EQ(projection.FillSourceLsn(0), 0);
  model::CashSnapshot expected, actual;
  ASSERT_TRUE(source.TryGetCash(expected));
  ASSERT_TRUE(projection.trading().TryGetCash(actual));
  EXPECT_EQ(actual, expected);
}

TEST_F(ArchiveProjectionTest, ReplayDrainsRequestsBeyondTheQueueCapacity) {
  ASSERT_TRUE(projection.Restore(Baseline()).ok());
  ASSERT_TRUE(mock.Prepare(config.trading).ok());
  ASSERT_TRUE(mock.Start(clock.Now()).ok());
  uint64_t lsn = 0;
  size_t fact_index = 0;
  for (size_t i = 0; i < 300; ++i) {
    source.BeginCallback(strategy::CallbackPhase::kTick);
    model::ClientOrderId id;
    ASSERT_EQ(
        source.SubmitOrder({config.market_data.instruments[0].instrument.id,
                            model::OrderSide::kBuy, 1, 1},
                           id),
        ErrorCode::kOk);
    source.EndCallback();
    ASSERT_TRUE(source.ProcessRequests(mock, clock.Now()).ok());
    for (; fact_index < sink.facts.size(); ++fact_index) {
      ASSERT_TRUE(
          projection
              .Replay(1, ++lsn, LedgerRecord(sink.facts[fact_index], 0, i))
              .ok());
    }
  }
  EXPECT_EQ(projection.orders().size(), 300);
  EXPECT_EQ(projection.trading().order_count(), 300);
}

TEST_F(ArchiveProjectionTest, ReplayPreservesStoppingLocalCancellations) {
  ASSERT_TRUE(projection.Restore(Baseline()).ok());
  source.BeginCallback(strategy::CallbackPhase::kTick);
  model::ClientOrderId id;
  ASSERT_EQ(source.SubmitOrder({config.market_data.instruments[0].instrument.id,
                                model::OrderSide::kBuy, 1000, 10},
                               id),
            ErrorCode::kOk);
  source.EndCallback();
  source.BeginStopping();
  ASSERT_TRUE(source.ProcessRequests(mock, clock.Now()).ok());
  trading::TradingDispatch dispatch;
  ASSERT_EQ(source.ApplyLocalUpdate(dispatch), ErrorCode::kOk);
  uint64_t lsn = 0;
  for (const auto& fact : sink.facts) {
    ASSERT_TRUE(projection.Replay(1, ++lsn, LedgerRecord(fact)).ok());
  }
  ASSERT_EQ(projection.orders().size(), 1);
  EXPECT_EQ(projection.orders()[0].snapshot.state,
            model::OrderState::kCanceled);
  ASSERT_TRUE(projection.has_update());
  EXPECT_EQ(projection.last_update().update.kind,
            model::OrderUpdateKind::kCanceledBeforeSend);
  model::CashSnapshot actual;
  ASSERT_TRUE(projection.trading().TryGetCash(actual));
  EXPECT_EQ(actual.reserved, 0);
}

TEST_F(ArchiveProjectionTest, CancelReportsTranslateCoreToLocalIdentity) {
  ASSERT_TRUE(projection.Restore(Baseline()).ok());
  SubmitAndFill();
  source.BeginCallback(strategy::CallbackPhase::kTick);
  ASSERT_EQ(source.CancelOrder({1}), ErrorCode::kOk);
  source.EndCallback();
  ASSERT_TRUE(source.ProcessRequests(mock, clock.Now()).ok());
  model::OrderSnapshot snapshot;
  ASSERT_TRUE(source.TryGetOrder({1}, snapshot));
  model::TradingEvent canceled;
  canceled.client_order_id = {1};
  canceled.adapter_order_id = snapshot.adapter_order_id;
  canceled.report_sequence = 3;
  canceled.kind = model::OrderUpdateKind::kCanceled;
  canceled.cumulative_filled_qty = 4;
  canceled.receive_time_ns = clock.Now();
  canceled.cancel_request_id = snapshot.cancel_request_id;
  trading::TradingDispatch dispatch;
  ASSERT_EQ(source.ApplyEvent(canceled, dispatch), ErrorCode::kOk);
  uint64_t lsn = 0;
  for (const auto& fact : sink.facts) {
    ASSERT_TRUE(projection.Replay(1, ++lsn, LedgerRecord(fact, 0, 0, 77)).ok());
  }
  ASSERT_TRUE(projection.has_update());
  EXPECT_EQ(projection.last_update().update.cancel_request_id.value, 77);
  EXPECT_EQ(projection.last_update().update.order.cancel_request_id.value, 77);
  EXPECT_EQ(projection.orders()[0].local_cancel_id.value, 77);
}

TEST_F(ArchiveProjectionTest, MetadataCanAddSessionsButCannotRewriteIdentity) {
  ASSERT_TRUE(projection.Restore(Baseline()).ok());
  ASSERT_TRUE(projection.LoadIdentity(Identity(true)).ok());
  ASSERT_EQ(projection.sessions().size(), trading::kMaxClients);
  EXPECT_EQ(projection.sessions()[1].client_id, 42);
  EXPECT_EQ(projection.sessions()[1].session_id, 4);
  EXPECT_EQ(projection.LoadIdentity(Identity(true, 99)).code,
            ErrorCode::kCorrupt);
  EXPECT_EQ(projection.sessions()[0].client_id, 41);
  EXPECT_EQ(projection.LoadIdentity(Identity()).code, ErrorCode::kCorrupt);
}

TEST_F(ArchiveProjectionTest, RejectsGapsAndWrongOwnerBeforeApplyingLedger) {
  ASSERT_TRUE(projection.Restore(Baseline()).ok());
  SubmitAndFill();
  EXPECT_EQ(projection.Replay(1, 2, LedgerRecord(sink.facts[0])).code,
            ErrorCode::kCorrupt);
  EXPECT_EQ(projection.Replay(1, 1, LedgerRecord(sink.facts[0], 1)).code,
            ErrorCode::kCorrupt);
  EXPECT_EQ(projection.Replay(1, 1, LedgerRecord(sink.facts[0], 0, 1)).code,
            ErrorCode::kCorrupt);
  EXPECT_EQ(projection.trading().order_count(), 0);
  EXPECT_EQ(projection.last_lsn(), 0);
}

TEST_F(ArchiveProjectionTest, LocalRejectionsRetainClientSequenceAndReason) {
  ASSERT_TRUE(projection.Restore(Baseline()).ok());
  trading::Message message;
  message.type = trading::MessageType::kOrderUpdate;
  message.sequence = 1;
  message.update.kind = model::OrderUpdateKind::kRejected;
  message.update.order.strategy_order_id = {1};
  message.update.order.instrument_id =
      config.market_data.instruments[0].instrument.id;
  message.update.order.side = model::OrderSide::kBuy;
  message.update.order.limit_price = 1000;
  message.update.order.original_qty = 10;
  message.update.order.state = model::OrderState::kRejected;
  message.update.order.report_sequence = 1;
  message.update.reason.Set(12, "insufficient cash");
  ASSERT_TRUE(projection.Replay(3, 1, RejectionRecord(message)).ok());
  ASSERT_EQ(projection.orders().size(), 1);
  EXPECT_EQ(projection.orders()[0].core_id.value, 0);
  ASSERT_TRUE(projection.has_update());
  EXPECT_EQ(projection.last_update().update.reason.view(), "insufficient cash");
  message.update.kind = model::OrderUpdateKind::kCancelRejected;
  message.update.order.report_sequence = 2;
  message.update.cancel_request_id = {7};
  message.update.order.cancel_request_id = {7};
  ASSERT_TRUE(projection.Replay(3, 2, RejectionRecord(message)).ok());
  EXPECT_EQ(projection.orders()[0].report_sequence, 2);
  EXPECT_EQ(projection.orders()[0].local_cancel_id.value, 7);
  EXPECT_EQ(projection.trading().order_count(), 0);
}

TEST(StoredStrategyReportTest, VersionThreeRejectionKeepsStrategyNamespace) {
  // Independent legacy v3 frame: rejected strategy order 7, no core order.
  std::array<std::byte, trading::kMaxFrameSize> bytes{};
  size_t offset = 0;
  const auto put = [&](uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
      bytes[offset++] = std::byte(value & 255);
      value >>= 8;
    }
  };
  put(0x52545159, 4);
  put(3, 2);
  put(5, 2);  // OrderUpdate.
  put(0, 4);  // Patched frame length below.
  put(0, 4);
  put(1, 8);
  put(1, 8);
  put(1, 1);  // Notify order.
  put(0, 1);  // No fill.
  put(7, 8);
  put(0, 1);  // Empty adapter identity.
  put((uint64_t{1} << 32) | 600000, 8);
  put(1, 1);  // Buy.
  put(100, 8);
  put(10, 8);
  put(0, 8);  // Cumulative quantity.
  put(0, 8);  // Leaves quantity.
  put(4, 1);  // Rejected.
  put(0, 1);  // No pending cancel.
  put(0, 8);
  put(1, 8);  // Report sequence.
  put(0, 8);
  put(0, 1);  // No position effect.
  put(1, 1);  // Rejection update kind.
  put(0, 8);
  put(0, 4);  // Empty diagnostic.
  put(0, 1);
  put(0, 1);
  put(1, 8);  // Account revision.
  put(0, 1);  // Cash account.
  put(0, 1);  // Not broker-managed.
  put(1000, 8);
  put(0, 8);
  put(1000, 8);
  put(0, 2);  // No positions.
  for (size_t i = 0; i < 4; ++i) {
    bytes[8 + i] = std::byte((offset >> (8 * i)) & 255);
  }
  trading::Message message;
  ASSERT_TRUE(
      DecodeStoredStrategyReport(std::span(bytes).first(offset), message).ok());
  EXPECT_EQ(message.update.order.strategy_order_id.value, 7U);
  EXPECT_EQ(message.update.order.client_order_id.value, 0U);
  const auto previous = message.update.order;
  EXPECT_FALSE(
      DecodeStoredStrategyReport(std::span(bytes).first(offset - 1), message)
          .ok());
  EXPECT_EQ(message.update.order, previous);
}

}  // namespace
}  // namespace yquant::tradingd
