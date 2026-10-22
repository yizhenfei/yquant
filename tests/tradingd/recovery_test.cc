#include "tradingd/recovery.h"

#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "trading_engine_harness.h"
#include "tradingd/journal.h"
#include "yquant/trading/trading_engine.h"

namespace yquant::tradingd {
namespace {

constexpr model::InstrumentId kInstrument{(1ULL << 32) | 600000};
constexpr std::array<trading::InstrumentSpec, 1> kInstruments = {
    {{kInstrument, 2}}};
constexpr std::array<DurableStrategyIdentity, 1> kIdentities = {{{42, 7}}};
constexpr BrokerRecoveryRequirements kRequirements{"mock/account-A/CNY", 10};

model::BrokerOrderId RequestKey(
    std::uint64_t identity = 42, std::uint64_t sequence = 9,
    ExternalRequestKind kind = ExternalRequestKind::kNewOrder) {
  char encoded[kExternalRequestKeyBytes + 1];
  EXPECT_TRUE(EncodeExternalRequestKey(identity, kind, sequence, encoded).ok());
  model::BrokerOrderId result;
  EXPECT_EQ(result.Assign(encoded), base::ErrorCode::kOk);
  return result;
}

// Independent counter lifetime: these facts remain after the gateway's entire
// journal object is destroyed. No mock-adapter checkpoint provides this truth.
class IndependentFakeBroker {
 public:
  IndependentFakeBroker() {
    BrokerRecoveryOrder order;
    order.request_key = RequestKey();
    EXPECT_EQ(order.broker_order_id.Assign("exchange-order-1"),
              base::ErrorCode::kOk);
    order.request = {kInstrument, model::OrderSide::kBuy, 1000, 10};
    order.state = model::OrderState::kFilled;
    order.cumulative_filled_qty = 10;
    order.fact_sequence = 100;
    orders.push_back(order);
    for (std::size_t index = 0; index < 2; ++index) {
      BrokerRecoveryFill fill;
      fill.request_key = order.request_key;
      fill.broker_order_id = order.broker_order_id;
      EXPECT_EQ(fill.execution_id.Assign(index == 0 ? "fill-1" : "fill-2"),
                base::ErrorCode::kOk);
      fill.instrument_id = kInstrument;
      fill.side = model::OrderSide::kBuy;
      fill.price = 1000;
      fill.quantity = index == 0 ? 4 : 6;
      fill.event_time_ns = 1700000000000000000ULL + index;
      fill.has_event_time = true;
      fill.fact_sequence = index == 0 ? 80 : 90;
      fills.push_back(fill);
    }
    account.cash = {900000000, 0, 900000000};
    account.position_count = 1;
    account.positions[0] = {kInstrument, 10, 0, 0};
  }

  BrokerRecoveryView Query() const {
    return {kRequirements.account_key,
            orders,
            fills,
            account,
            0,
            100,
            true,
            true,
            true,
            true};
  }
  std::vector<BrokerRecoveryOrder> orders;
  std::vector<BrokerRecoveryFill> fills;
  trading::AccountView account;
};

class Clock final : public base::MonotonicClock {
 public:
  std::uint64_t Now() const noexcept override { return 1000; }
};

TEST(BrokerRecoveryTest, RequestKeyRoundTripsWithoutTruncationOrAliasing) {
  for (const auto identity :
       std::array<std::uint64_t, 3>{1, 42, UINT64_MAX}) {
    for (const auto sequence :
         std::array<std::uint64_t, 3>{1, 9, UINT64_MAX}) {
      for (const auto kind :
           {ExternalRequestKind::kNewOrder, ExternalRequestKind::kCancel}) {
        char encoded[kExternalRequestKeyBytes + 1];
        ASSERT_TRUE(
            EncodeExternalRequestKey(identity, kind, sequence, encoded).ok());
        EXPECT_EQ(std::strlen(encoded), 28U);
        ExternalRequestKey decoded;
        ASSERT_TRUE(DecodeExternalRequestKey(encoded, decoded).ok());
        EXPECT_EQ(decoded.namespace_id, identity);
        EXPECT_EQ(decoded.kind, kind);
        EXPECT_EQ(decoded.sequence, sequence);
      }
    }
  }
  char encoded[kExternalRequestKeyBytes + 1] = "unchanged";
  EXPECT_FALSE(
      EncodeExternalRequestKey(0, ExternalRequestKind::kNewOrder, 1, encoded)
          .ok());
  EXPECT_STREQ(encoded, "unchanged");
  ASSERT_TRUE(
      EncodeExternalRequestKey(42, ExternalRequestKind::kNewOrder, 9, encoded)
          .ok());
  encoded[27] = encoded[27] == 'A' ? 'B' : 'R';
  ExternalRequestKey decoded{123, ExternalRequestKind::kCancel, 456};
  EXPECT_FALSE(DecodeExternalRequestKey(encoded, decoded).ok());
  EXPECT_EQ(decoded.namespace_id, 123U);
  EXPECT_EQ(decoded.sequence, 456U);
  EXPECT_FALSE(DecodeExternalRequestKey("invalid", decoded).ok());
}

TEST(BrokerRecoveryTest, RecoversFilledTerminalOrderAfterEntireWalDisappears) {
  IndependentFakeBroker broker;
  char directory[] = "/tmp/yquant-broker-recovery-XXXXXX";
  ASSERT_NE(mkdtemp(directory), nullptr);
  struct Cleanup {
    const char* path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{directory};
  {
    Journal journal;
    JournalOptions options;
    options.directory = directory;
    options.segment_bytes = kJournalMinSegmentBytes;
    options.sync_after_bytes = kJournalMinSyncAfterBytes;
    ASSERT_TRUE(journal.Acquire(options).ok());
    ASSERT_TRUE(journal.Start(1).ok());
    const std::array<std::byte, 4> bytes{};
    std::uint64_t lsn = 0;
    for (unsigned record = 0; record < 4; ++record)
      ASSERT_TRUE(journal.TryAppend(1, bytes, lsn).ok());
    ASSERT_TRUE(journal.Close().ok());
  }
  // The independent counter keeps its completed order and executions even
  // when the gateway's entire persisted state is unavailable.
  std::error_code removal_error;
  std::filesystem::remove_all(directory, removal_error);
  ASSERT_FALSE(removal_error);
  auto output = std::make_unique<RecoveredBrokerState>();
  BrokerRecoveryError error = BrokerRecoveryError::kInvalidFact;
  ASSERT_TRUE(BuildRecoveredBrokerState(broker.Query(), kRequirements,
                                        kIdentities, kInstruments, *output,
                                        &error)
                  .ok());
  EXPECT_EQ(error, BrokerRecoveryError::kNone);
  EXPECT_EQ(output->order_count, 1U);
  EXPECT_EQ(output->fill_count, 2U);
  EXPECT_EQ(output->orders[0].client_id, 7U);
  EXPECT_EQ(output->orders[0].identity.namespace_id, 42U);
  EXPECT_EQ(output->orders[0].identity.sequence, 9U);
  EXPECT_EQ(output->orders[0].fact.state, model::OrderState::kFilled);
  EXPECT_EQ(output->account.cash.balance, 900000000);
  EXPECT_EQ(output->account.positions[0].total_qty, 10);

  // Lease allocation is a separate cold-synced gateway operation. Map stable
  // broker identities to that new core range before installing the new image.
  constexpr model::ClientOrderId kLeasedCoreId{4097};
  model::OrderSnapshot order;
  const auto& fact = output->orders[0].fact;
  order.client_order_id = kLeasedCoreId;
  order.broker_order_id = fact.broker_order_id;
  order.instrument_id = fact.request.instrument_id;
  order.side = fact.request.side;
  order.limit_price = fact.request.limit_price;
  order.original_qty = fact.request.quantity;
  order.cumulative_filled_qty = fact.cumulative_filled_qty;
  order.state = fact.state;
  order.report_sequence = fact.fact_sequence;
  std::array<model::Fill, 2> fills;
  for (std::size_t index = 0; index < fills.size(); ++index) {
    const auto& execution = output->fills[index].fact;
    auto& fill = fills[index];
    fill.client_order_id = kLeasedCoreId;
    fill.broker_order_id = execution.broker_order_id;
    fill.execution_id = execution.execution_id;
    fill.instrument_id = execution.instrument_id;
    fill.side = execution.side;
    fill.price = execution.price;
    fill.quantity = execution.quantity;
    fill.event_time_ns = execution.event_time_ns;
    fill.has_event_time = execution.has_event_time;
    fill.receive_time_ns = 1000;
  }
  Clock clock;
  md::MarketDataConfig config;
  config.source_id = {1};

  config.instrument_count = 1;
  auto& instrument = config.instruments[0].instrument;
  instrument.id = kInstrument;
  instrument.venue = model::Venue::kSse;
  std::memcpy(instrument.symbol, "600000", model::kSymbolLength);
  instrument.price_decimals = 2;

  md::InstrumentScope market;
  ASSERT_TRUE(market.Configure(config).ok());
  testing::TradingEngineHarness ledger(clock, market);
  trading::TradingAccountSnapshot baseline;
  baseline.cash_balance = output->account.cash.balance;
  baseline.position_count = output->account.position_count;
  baseline.positions[0] = output->account.positions[0];
  ASSERT_TRUE(
      ledger.RestoreReconciled(baseline, std::span(&order, 1), fills).ok());
  model::CashSnapshot cash;
  ASSERT_TRUE(ledger.TryGetCash(cash));
  EXPECT_EQ(cash.balance, 900000000);
  EXPECT_EQ(cash.reserved, 0);
  EXPECT_EQ(ledger.order_count(), 1U);
  EXPECT_TRUE(ledger.is_drained());
  // An overlapping old callback must retain the imported account baseline.
  trading::TradingEvent duplicate;
  duplicate.client_order_id = kLeasedCoreId;
  duplicate.broker_order_id = order.broker_order_id;
  duplicate.kind = model::OrderUpdateKind::kFill;
  duplicate.report_sequence = order.report_sequence;
  duplicate.cumulative_filled_qty = 10;
  duplicate.receive_time_ns = 1000;
  duplicate.fill = fills[0];
  trading::TradingDispatch dispatch;
  EXPECT_EQ(ledger.ApplyEvent(duplicate, dispatch), base::ErrorCode::kOk);
  EXPECT_FALSE(dispatch.has_fill);
  EXPECT_FALSE(dispatch.notify_order);
  ASSERT_TRUE(ledger.TryGetCash(cash));
  EXPECT_EQ(cash.balance, 900000000);
}

TEST(BrokerRecoveryTest, DeduplicatesOverlappingOrdersAndDetailedExecutions) {
  IndependentFakeBroker broker;
  broker.orders.push_back(broker.orders[0]);
  broker.fills.push_back(broker.fills[0]);
  broker.fills.push_back(broker.fills[1]);
  auto output = std::make_unique<RecoveredBrokerState>();
  ASSERT_TRUE(BuildRecoveredBrokerState(broker.Query(), kRequirements,
                                        kIdentities, kInstruments, *output)
                  .ok());
  EXPECT_EQ(output->order_count, 1U);
  EXPECT_EQ(output->fill_count, 2U);
  EXPECT_EQ(output->fills[0].order_index, 0U);
  EXPECT_EQ(output->fills[1].order_index, 0U);
  EXPECT_EQ(output->account.cash.balance, 900000000);
}

TEST(BrokerRecoveryTest,
     IncompleteAndUnfencedQueriesBlockWithoutChangingOutput) {
  IndependentFakeBroker broker;
  auto output = std::make_unique<RecoveredBrokerState>();
  output->order_count = 999;
  output->account.cash.balance = 123;
  const auto check = [&](const BrokerRecoveryView& view,
                         BrokerRecoveryError expected) {
    BrokerRecoveryError error;
    EXPECT_FALSE(BuildRecoveredBrokerState(view, kRequirements, kIdentities,
                                           kInstruments, *output, &error)
                     .ok());
    EXPECT_EQ(error, expected);
    EXPECT_EQ(output->order_count, 999U);
    EXPECT_EQ(output->account.cash.balance, 123);
  };
  auto view = broker.Query();
  view.terminal_history_complete = false;
  view.orders = {};
  view.fills = {};
  check(view, BrokerRecoveryError::kIncompleteHistory);
  view = broker.Query();
  view.history_complete = false;
  check(view, BrokerRecoveryError::kIncompleteHistory);
  view = broker.Query();
  view.history_lower_bound = 11;
  check(view, BrokerRecoveryError::kIncompleteHistory);
  view = broker.Query();
  view.old_sender_fenced = false;
  check(view, BrokerRecoveryError::kSenderNotFenced);
  view = broker.Query();
  view.account_at_cut = false;
  check(view, BrokerRecoveryError::kIncompleteHistory);
  view = broker.Query();
  view.account_key = "another-account";
  check(view, BrokerRecoveryError::kAccountMismatch);
  view = broker.Query();
  view.fills = view.fills.first(1);
  check(view, BrokerRecoveryError::kIncompleteHistory);
}

TEST(BrokerRecoveryTest, UnknownNamespaceAndConflictingFactsBlockRecovery) {
  IndependentFakeBroker broker;
  auto output = std::make_unique<RecoveredBrokerState>();
  BrokerRecoveryError error;
  const auto check = [&](BrokerRecoveryError expected) {
    EXPECT_FALSE(BuildRecoveredBrokerState(broker.Query(), kRequirements,
                                           kIdentities, kInstruments, *output,
                                           &error)
                     .ok());
    EXPECT_EQ(error, expected);
  };
  broker.orders[0].request_key = RequestKey(43);
  check(BrokerRecoveryError::kUnknownOwner);
  broker.orders[0].request_key = RequestKey();
  broker.orders.push_back(broker.orders[0]);
  broker.orders[1].request.limit_price = 1001;
  check(BrokerRecoveryError::kConflictingOrder);
  broker.orders.pop_back();
  broker.fills.push_back(broker.fills[0]);
  broker.fills[2].quantity = 3;
  check(BrokerRecoveryError::kConflictingFill);
  broker.fills.pop_back();
  broker.fills[0].broker_order_id = {};
  check(BrokerRecoveryError::kInvalidFact);
}

TEST(BrokerRecoveryTest, ValidatesAccountDictionaryFillTotalsAndCut) {
  IndependentFakeBroker broker;
  auto output = std::make_unique<RecoveredBrokerState>();
  const auto valid_account = broker.account;
  broker.account.cash.available -= 1;
  EXPECT_FALSE(BuildRecoveredBrokerState(broker.Query(), kRequirements,
                                         kIdentities, kInstruments, *output)
                   .ok());
  broker.account = valid_account;
  broker.account.positions[0].instrument_id.value += 1;
  EXPECT_FALSE(BuildRecoveredBrokerState(broker.Query(), kRequirements,
                                         kIdentities, kInstruments, *output)
                   .ok());
  broker.account = valid_account;
  broker.fills[0].quantity = 5;
  EXPECT_FALSE(BuildRecoveredBrokerState(broker.Query(), kRequirements,
                                         kIdentities, kInstruments, *output)
                   .ok());
  broker.fills[0].quantity = 4;
  broker.fills[0].price = 1001;
  EXPECT_FALSE(BuildRecoveredBrokerState(broker.Query(), kRequirements,
                                         kIdentities, kInstruments, *output)
                   .ok());
  broker.fills[0].price = 1000;
  auto view = broker.Query();
  view.consistent_cut = 99;
  EXPECT_FALSE(BuildRecoveredBrokerState(view, kRequirements, kIdentities,
                                         kInstruments, *output)
                   .ok());
  constexpr std::array<DurableStrategyIdentity, 2> kDuplicateIdentity = {
      {{42, 7}, {42, 8}}};
  EXPECT_FALSE(BuildRecoveredBrokerState(broker.Query(), kRequirements,
                                         kDuplicateIdentity, kInstruments,
                                         *output)
                   .ok());
}

TEST(BrokerRecoveryTest,
     RejectsExecutionNotionalOverflowBeforeInstallingOutput) {
  IndependentFakeBroker broker;
  broker.orders[0].request.side = model::OrderSide::kSell;
  for (auto& fill : broker.fills) fill.side = model::OrderSide::kSell;
  broker.fills[0].price = std::numeric_limits<std::int64_t>::max();
  auto output = std::make_unique<RecoveredBrokerState>();
  output->account.cash.balance = 123;
  output->order_count = 17;
  BrokerRecoveryError error;
  EXPECT_FALSE(BuildRecoveredBrokerState(broker.Query(), kRequirements,
                                         kIdentities, kInstruments, *output,
                                         &error)
                   .ok());
  EXPECT_EQ(error, BrokerRecoveryError::kInvalidFact);
  EXPECT_EQ(output->account.cash.balance, 123);
  EXPECT_EQ(output->order_count, 17U);
}

TEST(BrokerRecoveryTest, IncludesWorkingOrdersWithoutDebitingAccountAgain) {
  IndependentFakeBroker broker;
  broker.orders[0].state = model::OrderState::kWorking;
  broker.orders[0].cumulative_filled_qty = 4;
  broker.fills.resize(1);
  broker.account.cash = {960000000, 60000000, 900000000};
  broker.account.positions[0].total_qty = 4;
  auto output = std::make_unique<RecoveredBrokerState>();
  ASSERT_TRUE(BuildRecoveredBrokerState(broker.Query(), kRequirements,
                                        kIdentities, kInstruments, *output)
                  .ok());
  EXPECT_EQ(output->orders[0].fact.state, model::OrderState::kWorking);
  EXPECT_EQ(output->account.cash.balance, 960000000);
  EXPECT_EQ(output->account.cash.reserved, 60000000);
  EXPECT_EQ(output->account.positions[0].total_qty, 4);
}

TEST(BrokerRecoveryTest, RejectsFuturesEffectsWithoutChangingCashOutput) {
  IndependentFakeBroker broker;
  auto output = std::make_unique<RecoveredBrokerState>();
  output->order_count = 17;
  output->account.cash.balance = 123;
  const auto check = [&] {
    BrokerRecoveryError error;
    EXPECT_FALSE(BuildRecoveredBrokerState(broker.Query(), kRequirements,
                                           kIdentities, kInstruments, *output,
                                           &error)
                     .ok());
    EXPECT_EQ(error, BrokerRecoveryError::kInvalidFact);
    EXPECT_EQ(output->order_count, 17U);
    EXPECT_EQ(output->account.cash.balance, 123);
  };
  broker.orders[0].request.position_effect = model::PositionEffect::kOpen;
  check();
  broker.orders[0].request.position_effect = model::PositionEffect::kNone;
  broker.fills[0].position_effect = model::PositionEffect::kCloseToday;
  check();
}

TEST(BrokerRecoveryTest, RejectsFuturesDictionaryBeforeReadingFacts) {
  IndependentFakeBroker broker;
  model::InstrumentId futures;
  ASSERT_EQ(model::MakeInstrumentId("CFFEX", "IF2612", futures),
            base::ErrorCode::kOk);
  const std::array<trading::InstrumentSpec, 1> instruments = {{{futures, 2}}};
  auto output = std::make_unique<RecoveredBrokerState>();
  output->order_count = 17;
  BrokerRecoveryError error;
  EXPECT_EQ(BuildRecoveredBrokerState(broker.Query(), kRequirements,
                                      kIdentities, instruments, *output, &error)
                .code,
            base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(error, BrokerRecoveryError::kInvalidRequirements);
  EXPECT_EQ(output->order_count, 17U);
}

}  // namespace
}  // namespace yquant::tradingd
