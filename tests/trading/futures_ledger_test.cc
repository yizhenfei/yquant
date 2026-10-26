#include "yquant/trading/futures_ledger.h"

#include <stddef.h>
#include <stdint.h>

#include <cstring>
#include <limits>
#include <optional>

#include "gtest/gtest.h"
#include "trading_engine_harness.h"
#include "yquant/market/instrument_scope.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_engine.h"

namespace yquant::trading {
namespace {
using base::ErrorCode;
using model::OrderSide;
using model::PositionEffect;
constexpr int64_t kYuan = 1000000;

class FuturesClock final : public base::MonotonicClock {
 public:
  uint64_t Now() const noexcept override { return 1000; }
};

class FuturesLedgerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config.source_id = {1};
    config.instrument_count = 1;
    auto& item = config.instruments[0];
    ASSERT_EQ(model::MakeInstrumentId("SHFE", "rb2701", id), ErrorCode::kOk);
    auto& instrument = item.instrument;
    instrument.id = id;
    instrument.venue = model::Venue::kShfe;
    std::memcpy(instrument.symbol, "rb2701", 6);
    instrument.type = model::InstrumentType::kFutures;
    instrument.price_decimals = 0;
    instrument.price_tick = 1;
    instrument.quantity_step = 1;
    instrument.contract_multiplier = 10;
    instrument.margin_rate_ppm = 100000;
    instrument.open_fee_per_lot = 2 * kYuan;
    instrument.close_today_fee_per_lot = 3 * kYuan;
    instrument.close_yesterday_fee_per_lot = kYuan;

    ASSERT_TRUE(market.Configure(config).ok());
    account.type = model::AccountType::kFutures;
    account.cash_balance = 100000 * kYuan;
  }

  void Configure() {
    ASSERT_EQ(ledger.Configure(account, &market), ErrorCode::kOk)
        << ledger.error_detail();
  }

  model::OrderSnapshot Order(OrderSide side, PositionEffect effect,
                             int64_t quantity, int64_t price = 100) const {
    model::OrderSnapshot order;
    order.instrument_id = id;
    order.side = side;
    order.position_effect = effect;
    order.limit_price = price;
    order.original_qty = order.leaves_qty = quantity;
    return order;
  }

  FuturesReservation Reserve(const model::OrderSnapshot& order) {
    FuturesReservation reservation;
    const model::OrderRequest request{order.instrument_id, order.side,
                                      order.limit_price, order.original_qty,
                                      order.position_effect};
    EXPECT_EQ(ledger.Reserve(request, reservation), ErrorCode::kOk)
        << ledger.error_detail();
    return reservation;
  }

  ErrorCode Fill(model::OrderSnapshot& order, FuturesReservation& reservation,
                 int64_t quantity, int64_t price) {
    model::Fill fill;
    fill.instrument_id = id;
    fill.side = order.side;
    fill.position_effect = order.position_effect;
    fill.quantity = quantity;
    fill.price = price;
    const auto result = ledger.ApplyFill(order, fill, reservation);
    if (result == ErrorCode::kOk) {
      order.leaves_qty -= quantity;
      order.cumulative_filled_qty += quantity;
    }
    return result;
  }

  model::FuturesAccountSnapshot Cash() const {
    model::FuturesAccountSnapshot out;
    EXPECT_TRUE(ledger.TryGetAccount(out));
    return out;
  }
  model::FuturesPositionSnapshot Position() const {
    model::FuturesPositionSnapshot out;
    EXPECT_TRUE(ledger.TryGetPosition(id, out));
    return out;
  }
  ErrorCode Mark(int64_t bid = 100, int64_t ask = 101) {
    model::QuoteTick quote;
    quote.instrument_id = id;
    quote.bid_price = bid;
    quote.ask_price = ask;
    quote.flags = static_cast<uint8_t>(model::QuoteFlag::kBidValid) |
                  static_cast<uint8_t>(model::QuoteFlag::kAskValid);
    return ledger.ApplyQuote(quote);
  }

  FuturesClock clock;
  market::InstrumentScope market;
  market::MarketDataConfig config;
  model::InstrumentId id;
  TradingAccountSnapshot account;
  FuturesLedger ledger;
};

TEST_F(FuturesLedgerTest, OpeningReservesMarginAndFeeRatherThanPrincipal) {
  Configure();
  ASSERT_EQ(Mark(), ErrorCode::kOk);
  auto order = Order(OrderSide::kBuy, PositionEffect::kOpen, 1);
  auto reservation = Reserve(order);
  EXPECT_EQ(reservation.cash, 102 * kYuan);
  EXPECT_EQ(Cash().balance, account.cash_balance);
  EXPECT_EQ(Cash().reserved, 102 * kYuan);
  ASSERT_EQ(Fill(order, reservation, 1, 100), ErrorCode::kOk);
  EXPECT_EQ(Cash().balance, account.cash_balance - 2 * kYuan);
  EXPECT_EQ(Cash().margin, 101 * kYuan);
  EXPECT_EQ(Cash().reserved, 0);
  EXPECT_EQ(Cash().commission, 2 * kYuan);
  EXPECT_EQ(Position().long_today.quantity, 1);
  EXPECT_EQ(Position().long_today.open_notional, 1000 * kYuan);
  EXPECT_EQ(Position().net_qty(), 1);
}

TEST_F(FuturesLedgerTest, PartialOpeningAndClosingUseExactFifoCost) {
  Configure();
  ASSERT_EQ(Mark(), ErrorCode::kOk);
  auto open = Order(OrderSide::kBuy, PositionEffect::kOpen, 3, 101);
  auto opening = Reserve(open);
  EXPECT_EQ(opening.cash, 309 * kYuan);
  ASSERT_EQ(Fill(open, opening, 2, 100), ErrorCode::kOk);
  EXPECT_EQ(opening.cash, 103 * kYuan);
  EXPECT_EQ(Cash().margin, 202 * kYuan);
  ASSERT_EQ(Fill(open, opening, 1, 101), ErrorCode::kOk);
  EXPECT_EQ(Cash().unrealized_pnl, -10 * kYuan);
  auto close = Order(OrderSide::kSell, PositionEffect::kCloseToday, 2, 102);
  auto closing = Reserve(close);
  EXPECT_EQ(Position().long_today.frozen_qty, 2);
  ASSERT_EQ(Fill(close, closing, 1, 103), ErrorCode::kOk);
  EXPECT_EQ(Cash().realized_pnl, 30 * kYuan);
  EXPECT_EQ(closing.cash, 3 * kYuan);
  ASSERT_EQ(Fill(close, closing, 1, 102), ErrorCode::kOk);
  EXPECT_EQ(Cash().realized_pnl, 50 * kYuan);
  EXPECT_EQ(Cash().commission, 12 * kYuan);
  EXPECT_EQ(Cash().balance, account.cash_balance + 38 * kYuan);
  EXPECT_EQ(Position().long_today.quantity, 1);
  EXPECT_EQ(Position().long_today.open_notional, 1010 * kYuan);
  EXPECT_EQ(Position().long_today.frozen_qty, 0);
  EXPECT_EQ(Cash().unrealized_pnl, -10 * kYuan);
  EXPECT_EQ(Cash().margin, 101 * kYuan);
}

TEST_F(FuturesLedgerTest, YesterdayClosingConsumesOnlyYesterdayInventory) {
  account.futures_position_count = 1;
  auto& initial = account.futures_positions[0];
  initial.instrument_id = id;
  initial.reference_price = 100;
  initial.long_yesterday = {2, 90};
  initial.long_today = {1, 110};
  Configure();
  auto close = Order(OrderSide::kSell, PositionEffect::kCloseYesterday, 1, 99);
  auto reservation = Reserve(close);
  ASSERT_EQ(Fill(close, reservation, 1, 99), ErrorCode::kOk);
  EXPECT_EQ(Cash().realized_pnl, 90 * kYuan);
  EXPECT_EQ(Cash().commission, kYuan);
  EXPECT_EQ(Position().long_yesterday.quantity, 1);
  EXPECT_EQ(Position().long_yesterday.open_notional, 900 * kYuan);
  EXPECT_EQ(Position().long_today.quantity, 1);
  EXPECT_EQ(Position().long_today.open_notional, 1100 * kYuan);
}

TEST_F(FuturesLedgerTest, ZeroNetInventoryStillUsesGrossMargin) {
  Configure();
  ASSERT_EQ(Mark(), ErrorCode::kOk);
  auto buy = Order(OrderSide::kBuy, PositionEffect::kOpen, 1, 100);
  auto sell = Order(OrderSide::kSell, PositionEffect::kOpen, 1, 101);
  auto buying = Reserve(buy);
  auto selling = Reserve(sell);
  ASSERT_EQ(Fill(buy, buying, 1, 100), ErrorCode::kOk);
  ASSERT_EQ(Fill(sell, selling, 1, 101), ErrorCode::kOk);
  EXPECT_EQ(Position().net_qty(), 0);
  EXPECT_EQ(Position().long_today.quantity, 1);
  EXPECT_EQ(Position().short_today.quantity, 1);
  EXPECT_EQ(Cash().margin, 202 * kYuan);
  EXPECT_EQ(Cash().commission, 4 * kYuan);
  EXPECT_EQ(Cash().unrealized_pnl, 0);
}

TEST_F(FuturesLedgerTest, InsufficientOpeningMarginPreservesClaimAndAccount) {
  account.cash_balance = 100 * kYuan;
  Configure();
  const auto before = Cash();
  FuturesReservation out{17, 23};
  model::OrderRequest request{id, OrderSide::kBuy, 100, 1,
                              PositionEffect::kOpen};
  EXPECT_EQ(ledger.Reserve(request, out), ErrorCode::kResourceExhausted);
  EXPECT_EQ(out.quantity, 17);
  EXPECT_EQ(out.cash, 23);
  EXPECT_EQ(Cash(), before);
  EXPECT_EQ(Position().net_qty(), 0);
}

TEST_F(FuturesLedgerTest, LosingMarkDisablesOpeningButStillAllowsClosing) {
  account.cash_balance = kYuan;
  account.futures_position_count = 1;
  auto& initial = account.futures_positions[0];
  initial.instrument_id = id;
  initial.reference_price = 100;
  initial.long_today = {1, 100};
  Configure();
  ASSERT_EQ(Mark(50, 51), ErrorCode::kOk);
  EXPECT_EQ(Cash().equity, -499 * kYuan);
  EXPECT_EQ(Cash().available, -550 * kYuan);
  FuturesReservation blocked;
  EXPECT_EQ(ledger.Reserve({id, OrderSide::kSell, 51, 1, PositionEffect::kOpen},
                           blocked),
            ErrorCode::kResourceExhausted);
  auto close = Order(OrderSide::kSell, PositionEffect::kCloseToday, 1, 50);
  auto reservation = Reserve(close);
  ASSERT_EQ(Fill(close, reservation, 1, 50), ErrorCode::kOk);
  EXPECT_EQ(Cash().balance, -502 * kYuan);
  EXPECT_EQ(Cash().available, Cash().balance);
  EXPECT_EQ(Cash().margin, 0);
  EXPECT_EQ(Position().net_qty(), 0);
}

TEST_F(FuturesLedgerTest, DefinitiveCancelReleasesOpeningAndClosingClaims) {
  account.futures_position_count = 1;
  auto& initial = account.futures_positions[0];
  initial.instrument_id = id;
  initial.reference_price = 100;
  initial.short_yesterday = {2, 100};
  Configure();
  auto close = Order(OrderSide::kBuy, PositionEffect::kCloseYesterday, 1);
  auto closing = Reserve(close);
  EXPECT_EQ(Position().short_yesterday.frozen_qty, 1);
  EXPECT_EQ(ledger.Release(close, closing), ErrorCode::kOk);
  EXPECT_EQ(Position().short_yesterday.frozen_qty, 0);
  auto open = Order(OrderSide::kSell, PositionEffect::kOpen, 1);
  auto opening = Reserve(open);
  EXPECT_GT(Cash().reserved, 0);
  EXPECT_EQ(ledger.Release(open, opening), ErrorCode::kOk);
  EXPECT_EQ(Cash().reserved, 0);
  EXPECT_EQ(opening.quantity, 0);
  EXPECT_EQ(closing.quantity, 0);
}

TEST_F(FuturesLedgerTest, OverflowingFillAndMarkPreserveFifoAndBalances) {
  Configure();
  auto open = Order(OrderSide::kSell, PositionEffect::kOpen, 1, 100);
  auto reservation = Reserve(open);
  const auto before = Cash();
  const auto position_before = Position();
  EXPECT_EQ(Fill(open, reservation, 1, std::numeric_limits<int64_t>::max()),
            ErrorCode::kCorrupt);
  EXPECT_EQ(Cash(), before);
  EXPECT_EQ(Position(), position_before);
  EXPECT_EQ(reservation.quantity, 1);
  ASSERT_EQ(Fill(open, reservation, 1, 100), ErrorCode::kOk);
  const auto after = Cash();
  const auto filled_position = Position();
  EXPECT_EQ(Mark(std::numeric_limits<int64_t>::max(),
                 std::numeric_limits<int64_t>::max()),
            ErrorCode::kCorrupt);
  EXPECT_EQ(Cash(), after);
  EXPECT_EQ(Position(), filled_position);
  auto close = Order(OrderSide::kBuy, PositionEffect::kCloseToday, 1, 99);
  auto closing = Reserve(close);
  ASSERT_EQ(Fill(close, closing, 1, 99), ErrorCode::kOk);
  EXPECT_EQ(Cash().realized_pnl, 10 * kYuan);
}

TEST_F(FuturesLedgerTest, RequiresExplicitEffectAndClosingInventory) {
  Configure();
  FuturesReservation reservation{3, 7};
  EXPECT_EQ(ledger.Reserve({id, OrderSide::kBuy, 100, 1}, reservation),
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(
      ledger.Reserve({id, OrderSide::kBuy, 100, 1, PositionEffect::kCloseToday},
                     reservation),
      ErrorCode::kResourceExhausted);
  EXPECT_EQ(reservation.quantity, 3);
  EXPECT_EQ(reservation.cash, 7);
}

TEST_F(FuturesLedgerTest, BootstrapRejectsInventoryOffQuantityStep) {
  market.Reset();
  config.instruments[0].instrument.quantity_step = 5;
  ASSERT_TRUE(market.Configure(config).ok());
  account.futures_position_count = 1;
  auto& position = account.futures_positions[0];
  position.instrument_id = id;
  position.reference_price = 100;
  position.long_today = {3, 100};
  EXPECT_EQ(ledger.Configure(account, &market), ErrorCode::kInvalidArgument);
  model::FuturesAccountSnapshot untouched;
  untouched.balance = 7;
  EXPECT_FALSE(ledger.TryGetAccount(untouched));
  EXPECT_EQ(untouched.balance, 7);
  position.long_today.quantity = 5;
  EXPECT_EQ(ledger.Configure(account, &market), ErrorCode::kOk);
  EXPECT_EQ(Position().long_today.quantity, 5);
}

TEST_F(FuturesLedgerTest, ReservationOverflowAndOffTickRequestsAreAtomic) {
  Configure();
  const auto before = Cash();
  FuturesReservation reservation{3, 7};
  EXPECT_EQ(
      ledger.Reserve({id, OrderSide::kBuy, std::numeric_limits<int64_t>::max(),
                      1, PositionEffect::kOpen},
                     reservation),
      ErrorCode::kInvalidArgument);
  EXPECT_EQ(reservation.quantity, 3);
  EXPECT_EQ(reservation.cash, 7);
  EXPECT_EQ(Cash(), before);
  ledger.Reset();
  market.Reset();
  config.instruments[0].instrument.price_tick = 2;
  config.instruments[0].instrument.quantity_step = 2;
  ASSERT_TRUE(market.Configure(config).ok());
  Configure();
  EXPECT_EQ(ledger.Reserve({id, OrderSide::kBuy, 101, 2, PositionEffect::kOpen},
                           reservation),
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(ledger.Reserve({id, OrderSide::kBuy, 100, 1, PositionEffect::kOpen},
                           reservation),
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(Cash(), before);
}

TEST_F(FuturesLedgerTest, OpeningMarginRoundsUpWithoutWideProductOverflow) {
  market.Reset();
  auto& instrument = config.instruments[0].instrument;
  instrument.price_decimals = 6;
  instrument.contract_multiplier = 1;
  instrument.margin_rate_ppm = 333333;
  instrument.open_fee_per_lot = 0;
  ASSERT_TRUE(market.Configure(config).ok());
  Configure();
  const auto order = Order(OrderSide::kBuy, PositionEffect::kOpen, 1, 1);
  const auto reservation = Reserve(order);
  EXPECT_EQ(reservation.cash, 1);
}

TEST_F(FuturesLedgerTest,
       FullOpeningFifoRejectsNewClaimWithoutReservationLeak) {
  account.cash_balance = 10000000 * kYuan;
  Configure();
  constexpr auto kCapacity =
      kMaxTradingFills + 4 * market::kMaxMarketDataInstruments;
  for (size_t i = 0; i != kCapacity; ++i) {
    auto order = Order(OrderSide::kBuy, PositionEffect::kOpen, 1);
    auto reservation = Reserve(order);
    ASSERT_EQ(Fill(order, reservation, 1, 100), ErrorCode::kOk) << i;
  }
  const auto before = Cash();
  FuturesReservation out{3, 7};
  EXPECT_EQ(
      ledger.Reserve({id, OrderSide::kBuy, 100, 1, PositionEffect::kOpen}, out),
      ErrorCode::kResourceExhausted);
  EXPECT_EQ(out.quantity, 3);
  EXPECT_EQ(out.cash, 7);
  EXPECT_EQ(Cash(), before);
  EXPECT_EQ(Position().long_today.quantity, kCapacity);
}

// Protocol coverage uses the real testing::TradingEngineHarness to prove
// duplicate executions cannot apply a second futures account/FIFO transaction.
class FuturesSendAdapter final : public TradingAdapter {
 public:
  base::Status Prepare(const TradingConfig&) noexcept override { return {}; }
  base::Status Start(uint64_t) noexcept override { return {}; }
  base::Status ReadAccountSnapshot(TradingAccountSnapshot&) noexcept override {
    return {};
  }
  base::Status SendOrder(model::ClientOrderId,
                         const model::OrderRequest& request,
                         uint64_t) noexcept override {
    sent_effect = request.position_effect;
    return {};
  }
  base::Status SendCancel(model::ClientOrderId, const model::AdapterOrderId&,
                          model::CancelRequestId, uint64_t) noexcept override {
    return {};
  }
  TradingPollResult Poll(uint64_t, model::TradingEvent&) noexcept override {
    return {};
  }
  std::optional<uint64_t> next_deadline() const noexcept override { return {}; }
  bool is_drained() const noexcept override { return true; }
  base::Status Stop() noexcept override { return {}; }
  const char* error_detail() const noexcept override { return ""; }
  PositionEffect sent_effect = PositionEffect::kNone;
};

TEST_F(FuturesLedgerTest,
       RealOrderLedgerDeduplicatesAndChecksEffectAtomically) {
  testing::TradingEngineHarness trading(clock, market);
  ASSERT_TRUE(trading.Configure(account).ok()) << trading.error_detail();
  model::CashSnapshot cash_sentinel{7, 2, 5};
  model::PositionSnapshot position_sentinel{id, 7, 2, 5};
  EXPECT_FALSE(trading.TryGetCash(cash_sentinel));
  EXPECT_FALSE(trading.TryGetPosition(id, position_sentinel));
  EXPECT_EQ(cash_sentinel.balance, 7);
  EXPECT_EQ(position_sentinel.total_qty, 7);
  trading.SetRunning(true);
  trading.BeginCallback(strategy::CallbackPhase::kTick);
  model::ClientOrderId order_id;
  ASSERT_EQ(trading.SubmitOrder(
                {id, OrderSide::kBuy, 100, 1, PositionEffect::kOpen}, order_id),
            ErrorCode::kOk);
  trading.EndCallback();
  FuturesSendAdapter adapter;
  ASSERT_TRUE(trading.ProcessRequests(adapter, 1000).ok());
  EXPECT_EQ(adapter.sent_effect, PositionEffect::kOpen);
  model::TradingEvent event;
  event.client_order_id = order_id;
  ASSERT_EQ(event.adapter_order_id.Assign("futures-order"), ErrorCode::kOk);
  event.kind = model::OrderUpdateKind::kFill;
  event.report_sequence = 1;
  event.cumulative_filled_qty = 1;
  event.receive_time_ns = 1000;
  event.fill.client_order_id = order_id;
  event.fill.adapter_order_id = event.adapter_order_id;
  ASSERT_EQ(event.fill.execution_id.Assign("fill-1"), ErrorCode::kOk);
  event.fill.instrument_id = id;
  event.fill.side = OrderSide::kBuy;
  event.fill.price = 100;
  event.fill.quantity = 1;
  event.fill.receive_time_ns = 1000;
  TradingDispatch dispatch;
  model::FuturesAccountSnapshot before;
  ASSERT_TRUE(trading.TryGetFuturesAccount(before));
  EXPECT_EQ(trading.ApplyEvent(event, dispatch), ErrorCode::kCorrupt);
  model::FuturesAccountSnapshot unchanged;
  ASSERT_TRUE(trading.TryGetFuturesAccount(unchanged));
  EXPECT_EQ(unchanged, before);
  event.fill.position_effect = PositionEffect::kOpen;
  ASSERT_EQ(trading.ApplyEvent(event, dispatch), ErrorCode::kOk)
      << trading.error_detail();
  EXPECT_TRUE(dispatch.has_fill);
  ASSERT_TRUE(trading.TryGetFuturesAccount(before));
  ASSERT_EQ(trading.ApplyEvent(event, dispatch), ErrorCode::kOk);
  EXPECT_FALSE(dispatch.has_fill);
  ASSERT_TRUE(trading.TryGetFuturesAccount(unchanged));
  EXPECT_EQ(unchanged, before);
  model::FuturesPositionSnapshot position;
  ASSERT_TRUE(trading.TryGetFuturesPosition(id, position));
  EXPECT_EQ(position.long_today.quantity, 1);
  EXPECT_EQ(before.commission, 2 * kYuan);
}

TEST_F(FuturesLedgerTest, FullRequestQueueDoesNotReserveAnotherFuturesOrder) {
  testing::TradingEngineHarness trading(clock, market);
  ASSERT_TRUE(trading.Configure(account).ok());
  trading.SetRunning(true);
  trading.BeginCallback(strategy::CallbackPhase::kTick);
  const model::OrderRequest request{id, OrderSide::kBuy, 100, 1,
                                    PositionEffect::kOpen};
  for (size_t i = 0; i != kMaxTradingRequests; ++i) {
    model::ClientOrderId id;
    ASSERT_EQ(trading.SubmitOrder(request, id), ErrorCode::kOk) << i;
  }
  model::FuturesAccountSnapshot before;
  ASSERT_TRUE(trading.TryGetFuturesAccount(before));
  model::ClientOrderId untouched{12345};
  EXPECT_EQ(trading.SubmitOrder(request, untouched),
            ErrorCode::kResourceExhausted);
  model::FuturesAccountSnapshot after;
  ASSERT_TRUE(trading.TryGetFuturesAccount(after));
  EXPECT_EQ(after, before);
  EXPECT_EQ(untouched.value, 12345);
  EXPECT_EQ(trading.order_count(), kMaxTradingRequests);
  trading.EndCallback();
}

TEST_F(FuturesLedgerTest, LocalCancellationReleasesFuturesClaimBeforeSend) {
  testing::TradingEngineHarness trading(clock, market);
  ASSERT_TRUE(trading.Configure(account).ok());
  trading.SetRunning(true);
  trading.BeginCallback(strategy::CallbackPhase::kTick);
  model::ClientOrderId order_id;
  ASSERT_EQ(trading.SubmitOrder(
                {id, OrderSide::kBuy, 100, 2, PositionEffect::kOpen}, order_id),
            ErrorCode::kOk);
  trading.EndCallback();
  trading.BeginStopping();
  FuturesSendAdapter adapter;
  ASSERT_TRUE(trading.ProcessRequests(adapter, 1000).ok());
  EXPECT_EQ(adapter.sent_effect, PositionEffect::kNone);
  TradingDispatch dispatch;
  ASSERT_EQ(trading.ApplyLocalUpdate(dispatch), ErrorCode::kOk);
  EXPECT_EQ(dispatch.update.kind, model::OrderUpdateKind::kCanceledBeforeSend);
  EXPECT_EQ(dispatch.update.order.position_effect, PositionEffect::kOpen);
  model::FuturesAccountSnapshot after;
  ASSERT_TRUE(trading.TryGetFuturesAccount(after));
  EXPECT_EQ(after.balance, account.cash_balance);
  EXPECT_EQ(after.reserved, 0);
  EXPECT_EQ(after.commission, 0);
  EXPECT_TRUE(trading.is_drained());
}

TEST_F(FuturesLedgerTest,
       EquityOrdersAndExecutionsHonorExplicitSpecifications) {
  market::MarketDataConfig stock_config;
  stock_config.source_id = {1};

  stock_config.instrument_count = 1;
  auto& item = stock_config.instruments[0];
  model::InstrumentId stock_id;
  ASSERT_EQ(model::MakeInstrumentId("SSE", "600000", stock_id), ErrorCode::kOk);
  item.instrument.id = stock_id;
  item.instrument.venue = model::Venue::kSse;
  std::memcpy(item.instrument.symbol, "600000", 6);
  item.instrument.price_decimals = 2;
  item.instrument.price_tick = 2;
  item.instrument.quantity_step = 2;

  market::InstrumentScope stock_market;
  ASSERT_TRUE(stock_market.Configure(stock_config).ok());
  TradingAccountSnapshot initial;
  initial.cash_balance = 1000000 * kYuan;
  initial.position_count = 1;
  initial.positions[0] = {stock_id, 3, 2, 0};
  testing::TradingEngineHarness trading(clock, stock_market);
  EXPECT_EQ(trading.Configure(initial).code, ErrorCode::kInvalidArgument);
  initial.positions[0] = {stock_id, 4, 3, 0};
  EXPECT_EQ(trading.Configure(initial).code, ErrorCode::kInvalidArgument);
  initial.positions[0].available_qty = 4;
  ASSERT_TRUE(trading.Configure(initial).ok());
  trading.SetRunning(true);
  trading.BeginCallback(strategy::CallbackPhase::kTick);
  model::ClientOrderId order_id{12345};
  EXPECT_EQ(trading.SubmitOrder({stock_id, OrderSide::kBuy, 1001, 2}, order_id),
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(trading.SubmitOrder({stock_id, OrderSide::kBuy, 1000, 1}, order_id),
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(order_id.value, 12345);
  ASSERT_EQ(trading.SubmitOrder({stock_id, OrderSide::kBuy, 1000, 2}, order_id),
            ErrorCode::kOk);
  trading.EndCallback();
  FuturesSendAdapter adapter;
  ASSERT_TRUE(trading.ProcessRequests(adapter, 1000).ok());
  model::TradingEvent event;
  event.client_order_id = order_id;
  ASSERT_EQ(event.adapter_order_id.Assign("stock-order"), ErrorCode::kOk);
  event.kind = model::OrderUpdateKind::kFill;
  event.report_sequence = 1;
  event.cumulative_filled_qty = 1;
  event.receive_time_ns = 1000;
  event.fill.client_order_id = order_id;
  event.fill.adapter_order_id = event.adapter_order_id;
  ASSERT_EQ(event.fill.execution_id.Assign("stock-fill"), ErrorCode::kOk);
  event.fill.instrument_id = stock_id;
  event.fill.side = OrderSide::kBuy;
  event.fill.price = 1000;
  event.fill.quantity = 1;
  event.fill.receive_time_ns = 1000;
  model::CashSnapshot before;
  ASSERT_TRUE(trading.TryGetCash(before));
  TradingDispatch dispatch;
  EXPECT_EQ(trading.ApplyEvent(event, dispatch), ErrorCode::kCorrupt);
  event.fill.price = 999;
  event.fill.quantity = event.cumulative_filled_qty = 2;
  EXPECT_EQ(trading.ApplyEvent(event, dispatch), ErrorCode::kCorrupt);
  model::CashSnapshot after;
  ASSERT_TRUE(trading.TryGetCash(after));
  EXPECT_EQ(after, before);
  event.fill.price = 1000;
  EXPECT_EQ(trading.ApplyEvent(event, dispatch), ErrorCode::kOk);
  ASSERT_TRUE(trading.TryGetCash(after));
  EXPECT_EQ(after.balance, initial.cash_balance - 20 * kYuan);
}

TEST_F(FuturesLedgerTest, RuntimeDispatchMarksFuturesAccountBeforeQueries) {
  testing::TradingEngineHarness trading(clock, market);
  account.futures_position_count = 1;
  auto& initial = account.futures_positions[0];
  initial.instrument_id = id;
  initial.reference_price = 100;
  initial.long_today = {1, 100};
  ASSERT_TRUE(trading.Configure(account).ok());
  testing::TradingEngineHarness& runtime = trading;
  model::QuoteTick quote;
  quote.instrument_id = id;
  quote.bid_price = 102;
  quote.ask_price = 103;
  quote.flags = static_cast<uint8_t>(model::QuoteFlag::kBidValid) |
                static_cast<uint8_t>(model::QuoteFlag::kAskValid);
  ASSERT_EQ(runtime.ApplyQuote(quote), ErrorCode::kOk);
  model::FuturesAccountSnapshot snapshot;
  ASSERT_TRUE(runtime.TryGetFuturesAccount(snapshot));
  EXPECT_EQ(snapshot.unrealized_pnl, 20 * kYuan);
  EXPECT_EQ(snapshot.margin, 103 * kYuan);
  EXPECT_EQ(snapshot.equity, 100020 * kYuan);
}

// A broker supplies complete snapshots separately from normalized order facts.
// The fake never computes fees, margin, freezes or simulated executions.
class BrokerSnapshotAdapter final : public TradingAdapter {
 public:
  BrokerAccountSnapshot account;
  model::TradingEvent fact;
  TradingPollState pending = TradingPollState::kIdle;
  int stopping_calls = 0;
  void BeginStopping() noexcept override { ++stopping_calls; }
  base::Status Prepare(const TradingConfig&) noexcept override { return {}; }
  base::Status Start(uint64_t) noexcept override { return {}; }
  base::Status ReadAccountSnapshot(TradingAccountSnapshot&) noexcept override {
    return {ErrorCode::kUnsupported, {}};
  }
  base::Status ReadBrokerAccountSnapshot(
      BrokerAccountSnapshot& out) noexcept override {
    out = account;
    return {};
  }
  base::Status SendOrder(model::ClientOrderId, const model::OrderRequest&,
                         uint64_t) noexcept override {
    return {};
  }
  base::Status SendCancel(model::ClientOrderId, const model::AdapterOrderId&,
                          model::CancelRequestId, uint64_t) noexcept override {
    return {};
  }
  TradingPollResult Poll(uint64_t, model::TradingEvent& out) noexcept override {
    const auto state = pending;
    pending = TradingPollState::kIdle;
    if (state == TradingPollState::kEvent) {
      out = fact;
    }
    return {state, {}};
  }
  std::optional<uint64_t> next_deadline() const noexcept override {
    return pending == TradingPollState::kIdle ? std::nullopt
                                              : std::optional<uint64_t>{0};
  }
  bool is_drained() const noexcept override {
    return pending == TradingPollState::kIdle;
  }
  base::Status Stop() noexcept override { return {}; }
  const char* error_detail() const noexcept override { return "broker fake"; }
};

TEST_F(FuturesLedgerTest,
       BrokerBootstrapAndPureUpdatesPreserveActualCostAndMoney) {
  BrokerSnapshotAdapter adapter;
  adapter.account.revision = 1;
  adapter.account.account = {93000000, 100000000, 43000000, 8000000,
                             6000000,  0,         7000000,  12345};
  adapter.account.position_count = 1;
  adapter.account.positions[0].instrument_id = id;
  // Two lots opened at 100 and 101 cannot become a single tick-aligned price.
  adapter.account.positions[0].long_today = {2, 0, 2010000000};
  adapter.account.positions[0].long_yesterday = {1, 0, 1000000000};
  adapter.account.positions[0].unallocated_long_frozen = 1;
  TradingEngine engine(&clock, &market, &adapter);
  TradingConfig broker;
  broker.adapter = TradingAdapterKind::kCtp;
  ASSERT_TRUE(engine.Init(broker, config).ok()) << engine.error_detail();
  EXPECT_TRUE(engine.broker_managed());
  model::FuturesAccountSnapshot actual;
  model::FuturesPositionSnapshot position;
  ASSERT_TRUE(engine.TryGetFuturesAccount(actual));
  EXPECT_EQ(actual, adapter.account.account);
  ASSERT_TRUE(engine.TryGetFuturesPosition(id, position));
  EXPECT_EQ(position, adapter.account.positions[0]);
  model::QuoteTick quote;
  quote.instrument_id = id;
  quote.bid_price = 900;
  quote.ask_price = 1000;
  quote.flags = 7;
  ASSERT_EQ(engine.ApplyQuote(quote), ErrorCode::kOk);
  ASSERT_TRUE(engine.TryGetFuturesAccount(actual));
  EXPECT_EQ(actual, adapter.account.account);
  engine.SetAcceptingRequests(true);
  model::ClientOrderId order;
  ASSERT_EQ(engine.SubmitOrder(
                {id, OrderSide::kBuy, 100, 1, PositionEffect::kOpen}, order),
            ErrorCode::kOk);
  engine.SetAcceptingRequests(false);
  ASSERT_TRUE(engine.TryGetFuturesAccount(actual));
  EXPECT_EQ(actual, adapter.account.account);
  adapter.account.revision = 2;
  adapter.account.account.available = 12000000;
  adapter.account.positions[0].unallocated_long_frozen = 2;
  adapter.pending = TradingPollState::kAccount;
  TradingDispatch dispatch;
  bool event = false;
  ASSERT_TRUE(engine.Poll(1000, dispatch, event).ok());
  EXPECT_TRUE(event);
  EXPECT_FALSE(dispatch.notify_order);
  EXPECT_EQ(dispatch.update.order.client_order_id.value, 0);
  ASSERT_TRUE(engine.TryGetFuturesAccount(actual));
  EXPECT_EQ(actual.available, 12000000);
  ASSERT_TRUE(engine.TryGetFuturesPosition(id, position));
  EXPECT_EQ(position.unallocated_long_frozen, 2);
  adapter.account.account.available = 7;
  adapter.pending = TradingPollState::kAccount;
  EXPECT_FALSE(engine.Poll(1000, dispatch, event).ok());
  ASSERT_TRUE(engine.TryGetFuturesAccount(actual));
  EXPECT_EQ(actual.available, 12000000);
}

TEST_F(FuturesLedgerTest, BrokerExternalCancelNeedsNoSyntheticLocalCancel) {
  BrokerSnapshotAdapter adapter;
  adapter.account.revision = 1;
  TradingEngine engine(&clock, &market, &adapter);
  TradingConfig broker;
  broker.adapter = TradingAdapterKind::kCtp;
  ASSERT_TRUE(engine.Init(broker, config).ok());
  engine.SetAcceptingRequests(true);
  model::ClientOrderId order;
  ASSERT_EQ(engine.SubmitOrder(
                {id, OrderSide::kBuy, 100, 1, PositionEffect::kOpen}, order),
            ErrorCode::kOk);
  engine.SetAcceptingRequests(false);
  ASSERT_TRUE(engine.ProcessRequests(1000).ok());
  adapter.fact.client_order_id = order;
  ASSERT_EQ(adapter.fact.adapter_order_id.Assign("ctp-order"), ErrorCode::kOk);
  adapter.fact.report_sequence = 1;
  adapter.fact.kind = model::OrderUpdateKind::kCanceled;
  adapter.fact.receive_time_ns = 1000;
  adapter.pending = TradingPollState::kEvent;
  TradingDispatch dispatch;
  bool event = false;
  ASSERT_TRUE(engine.Poll(1000, dispatch, event).ok()) << engine.error_detail();
  EXPECT_TRUE(event);
  EXPECT_EQ(dispatch.update.order.state, model::OrderState::kCanceled);
  EXPECT_EQ(dispatch.update.cancel_request_id.value, 0);
  EXPECT_TRUE(engine.is_drained());
}

TEST_F(FuturesLedgerTest,
       BrokerUnknownHoldingAndOverallocatedFreezeRejectBootstrap) {
  BrokerSnapshotAdapter adapter;
  adapter.account.revision = 1;
  adapter.account.position_count = 1;
  ASSERT_EQ(model::MakeInstrumentId("DCE", "a2701",
                                    adapter.account.positions[0].instrument_id),
            ErrorCode::kOk);
  TradingConfig broker;
  broker.adapter = TradingAdapterKind::kCtp;
  TradingEngine unknown(&clock, &market, &adapter);
  EXPECT_FALSE(unknown.Init(broker, config).ok());
  adapter.account.positions[0].instrument_id = id;
  adapter.account.positions[0].long_today.quantity = 1;
  adapter.account.positions[0].unallocated_long_frozen = 2;
  TradingEngine bad_freeze(&clock, &market, &adapter);
  EXPECT_FALSE(bad_freeze.Init(broker, config).ok());
}

TEST_F(FuturesLedgerTest, BrokerEventValidatesAccountBeforeCommittingOrder) {
  BrokerSnapshotAdapter adapter;
  adapter.account.revision = 1;
  TradingEngine engine(&clock, &market, &adapter);
  TradingConfig broker;
  broker.adapter = TradingAdapterKind::kCtp;
  ASSERT_TRUE(engine.Init(broker, config).ok());
  engine.SetAcceptingRequests(true);
  model::ClientOrderId order;
  ASSERT_EQ(engine.SubmitOrder(
                {id, OrderSide::kBuy, 100, 1, PositionEffect::kOpen}, order),
            ErrorCode::kOk);
  engine.SetAcceptingRequests(false);
  ASSERT_TRUE(engine.ProcessRequests(1000).ok());
  adapter.fact.client_order_id = order;
  adapter.fact.report_sequence = 1;
  adapter.fact.receive_time_ns = 1000;
  ASSERT_EQ(adapter.fact.adapter_order_id.Assign("ctp-accepted"),
            ErrorCode::kOk);
  adapter.account.revision = 2;
  adapter.account.position_count = 1;
  ASSERT_EQ(model::MakeInstrumentId("DCE", "a2701",
                                    adapter.account.positions[0].instrument_id),
            ErrorCode::kOk);
  adapter.pending = TradingPollState::kEvent;
  TradingDispatch dispatch;
  bool event = false;
  EXPECT_FALSE(engine.Poll(1000, dispatch, event).ok());
  model::OrderSnapshot retained;
  ASSERT_TRUE(engine.TryGetOrder(order, retained));
  EXPECT_EQ(retained.state, model::OrderState::kPendingNew);
  EXPECT_EQ(retained.report_sequence, 0);
}

TEST_F(FuturesLedgerTest, BrokerGenericCloseUsesExchangeAllocationOnly) {
  auto generic = config;
  auto& instrument = generic.instruments[0].instrument;
  ASSERT_EQ(model::MakeInstrumentId("DCE", "a2701", instrument.id),
            ErrorCode::kOk);
  instrument.venue = model::Venue::kDce;
  std::memset(instrument.symbol, 0, sizeof(instrument.symbol));
  std::memcpy(instrument.symbol, "a2701", 5);
  market::InstrumentScope generic_market;
  ASSERT_TRUE(generic_market.Configure(generic).ok());
  BrokerSnapshotAdapter adapter;
  adapter.account.revision = 1;
  TradingEngine engine(&clock, &generic_market, &adapter);
  TradingConfig broker;
  broker.adapter = TradingAdapterKind::kCtp;
  ASSERT_TRUE(engine.Init(broker, generic).ok());
  engine.SetAcceptingRequests(true);
  model::ClientOrderId order{99};
  EXPECT_EQ(engine.SubmitOrder({instrument.id, OrderSide::kSell, 100, 1,
                                PositionEffect::kCloseToday},
                               order),
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(order.value, 99);
  // Inventory/funds remain broker risk decisions; no simulated reservation.
  ASSERT_EQ(engine.SubmitOrder({instrument.id, OrderSide::kSell, 100, 1,
                                PositionEffect::kClose},
                               order),
            ErrorCode::kOk);
  model::FuturesAccountSnapshot actual;
  ASSERT_TRUE(engine.TryGetFuturesAccount(actual));
  EXPECT_EQ(actual, adapter.account.account);
}

TEST_F(FuturesLedgerTest, BrokerShutdownAnnouncedOnceWithoutTransportWork) {
  BrokerSnapshotAdapter adapter;
  adapter.account.revision = 1;
  TradingEngine engine(&clock, &market, &adapter);
  TradingConfig broker;
  broker.adapter = TradingAdapterKind::kCtp;
  ASSERT_TRUE(engine.Init(broker, config).ok());
  engine.BeginStopping();
  engine.BeginStopping();
  EXPECT_EQ(adapter.stopping_calls, 1);
  EXPECT_EQ(adapter.pending, TradingPollState::kIdle);
}

}  // namespace
}  // namespace yquant::trading
