#include "yquant/trading/futures_ledger.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <new>
#include <optional>
#include <utility>

#include "yquant/base/format.h"
#include "yquant/market/instrument_scope.h"
#include "yquant/trading/trading_limits.h"

namespace yquant::trading {
namespace {
using base::ErrorCode;
using model::PositionEffect;
constexpr auto kMax = std::numeric_limits<int64_t>::max();
constexpr auto kMin = std::numeric_limits<int64_t>::min();
constexpr uint32_t kNoLot = UINT32_MAX;
constexpr size_t kMaxOpeningLots =
    kMaxTradingFills + 4 * market::kMaxMarketDataInstruments;

// Checked integer primitives preserve out on failure. Signed PnL/available
// values may be negative; notionals, fees and quantities may not.
bool Add(int64_t a, int64_t b, int64_t& out) noexcept {
  if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b)) {
    return false;
  }
  out = a + b;
  return true;
}
bool Sub(int64_t a, int64_t b, int64_t& out) noexcept {
  if ((b > 0 && a < kMin + b) || (b < 0 && a > kMax + b)) {
    return false;
  }
  out = a - b;
  return true;
}
bool Mul(int64_t a, int64_t b, int64_t& out) noexcept {
  if (a < 0 || b < 0 || (b != 0 && a > kMax / b)) {
    return false;
  }
  out = a * b;
  return true;
}

struct LotQueue {
  uint32_t head = kNoLot;
  uint32_t tail = kNoLot;
};
struct OpeningLot {
  int64_t quantity = 0;
  int64_t unit_notional = 0;
  uint32_t next = kNoLot;
};
struct PositionRecord {
  model::InstrumentDefinition instrument;
  model::FuturesPositionSnapshot position;
  std::array<LotQueue, 4> queues{};
  int64_t bid = 0;
  int64_t ask = 0;
};

// Bucket order is long today/yesterday, then short today/yesterday.
model::FuturesPositionBucket& Bucket(model::FuturesPositionSnapshot& position,
                                     size_t index) noexcept {
  switch (index) {
    case 0:
      return position.long_today;
    case 1:
      return position.long_yesterday;
    case 2:
      return position.short_today;
    default:
      return position.short_yesterday;
  }
}

// Computes an exact micro-yuan notional with individually checked products.
// A zero quantity has zero notional even before the first mark is known.
bool Notional(const model::InstrumentDefinition& instrument, int64_t price,
              int64_t quantity, int64_t& out) noexcept {
  if (quantity == 0) {
    out = 0;
    return true;
  }
  if (price <= 0 || quantity < 0 || instrument.price_decimals > 6) {
    return false;
  }
  int64_t scale = 1;
  for (uint8_t i = instrument.price_decimals; i < 6; ++i) {
    scale *= 10;
  }
  int64_t value;
  return Mul(price, instrument.contract_multiplier, value) &&
         Mul(value, scale, value) && Mul(value, quantity, out);
}

// Splitting quotient/remainder avoids overflowing notional * rate. Valid rates
// are at most one million, so the rounded margin never exceeds the notional.
bool Margin(int64_t notional, int64_t rate, int64_t& out) noexcept {
  if (notional < 0 || rate < 1 || rate > 1000000) {
    return false;
  }
  const auto whole = (notional / 1000000) * rate;
  const auto remainder = (notional % 1000000) * rate;
  return Add(whole, remainder / 1000000 + (remainder % 1000000 != 0), out);
}

// Returns whether the simulated ledger supports this opening/closing intent.
bool ValidEffect(PositionEffect effect) noexcept {
  switch (effect) {
    case PositionEffect::kOpen:
    case PositionEffect::kCloseToday:
    case PositionEffect::kCloseYesterday:
      return true;
    case PositionEffect::kNone:
    case PositionEffect::kClose:
      return false;
  }
  return false;
}

// Selects the direction/day bucket for a request with a validated effect/side.
size_t OrderBucket(model::OrderSide side, PositionEffect effect) noexcept {
  switch (effect) {
    case PositionEffect::kOpen:
      return side == model::OrderSide::kBuy ? 0 : 2;
    case PositionEffect::kCloseToday:
      return side == model::OrderSide::kBuy ? 2 : 0;
    case PositionEffect::kCloseYesterday:
      return side == model::OrderSide::kBuy ? 3 : 1;
    case PositionEffect::kNone:
    case PositionEffect::kClose:
      break;
  }
  return 0;
}

// Returns per-lot fees for an intent already checked by ValidEffect().
int64_t Fee(const model::InstrumentDefinition& instrument,
            PositionEffect effect) noexcept {
  switch (effect) {
    case PositionEffect::kOpen:
      return instrument.open_fee_per_lot;
    case PositionEffect::kCloseToday:
      return instrument.close_today_fee_per_lot;
    case PositionEffect::kCloseYesterday:
      return instrument.close_yesterday_fee_per_lot;
    case PositionEffect::kNone:
    case PositionEffect::kClose:
      break;
  }
  return 0;
}

// Initializes only a temporary position; dictionary values are copied once.
bool NewPosition(const market::InstrumentScope& market, model::InstrumentId id,
                 PositionRecord& out) noexcept {
  model::InstrumentDefinition instrument;
  if (!market.TryGetInstrument(id, instrument) ||
      instrument.type != model::InstrumentType::kFutures ||
      instrument.price_decimals > 6 || instrument.price_tick <= 0 ||
      instrument.quantity_step <= 0 || instrument.contract_multiplier <= 0 ||
      instrument.margin_rate_ppm < 1 || instrument.margin_rate_ppm > 1000000 ||
      instrument.open_fee_per_lot < 0 ||
      instrument.close_today_fee_per_lot < 0 ||
      instrument.close_yesterday_fee_per_lot < 0) {
    return false;
  }
  out = {};
  out.instrument = instrument;
  out.position.instrument_id = id;
  return true;
}

// Revalues a temporary position. Gross quantity and each exact open/marked
// notional must fit int64; no long/short margin offsets are assumed.
bool Revalue(PositionRecord& record) noexcept {
  int64_t gross = 0;
  int64_t pnl = 0;
  for (size_t i = 0; i != 4; ++i) {
    const auto& bucket = Bucket(record.position, i);
    if (bucket.quantity < 0 || bucket.frozen_qty < 0 ||
        bucket.frozen_qty > bucket.quantity || bucket.open_notional < 0 ||
        (bucket.quantity == 0 && bucket.open_notional != 0) ||
        !Add(gross, bucket.quantity, gross)) {
      return false;
    }
    int64_t marked;
    int64_t part;
    if (!Notional(record.instrument, i < 2 ? record.bid : record.ask,
                  bucket.quantity, marked) ||
        !(i < 2 ? Sub(marked, bucket.open_notional, part)
                : Sub(bucket.open_notional, marked, part)) ||
        !Add(pnl, part, pnl)) {
      return false;
    }
  }
  int64_t marked_gross;
  int64_t margin;
  if (!Notional(record.instrument, std::max(record.bid, record.ask), gross,
                marked_gross) ||
      !Margin(marked_gross, record.instrument.margin_rate_ppm, margin)) {
    return false;
  }
  record.position.margin = margin;
  record.position.unrealized_pnl = pnl;
  return true;
}

// Replaces one position's contribution in a temporary account and derives the
// signed equity/available fields. Does not impose a nonnegative available gate.
bool RevalueAccount(model::FuturesAccountSnapshot& account,
                    const model::FuturesPositionSnapshot& before,
                    const model::FuturesPositionSnapshot& after) noexcept {
  return Sub(account.margin, before.margin, account.margin) &&
         Add(account.margin, after.margin, account.margin) &&
         Sub(account.unrealized_pnl, before.unrealized_pnl,
             account.unrealized_pnl) &&
         Add(account.unrealized_pnl, after.unrealized_pnl,
             account.unrealized_pnl) &&
         Add(account.balance, account.unrealized_pnl, account.equity) &&
         Sub(account.equity, account.margin, account.available) &&
         Sub(account.available, account.reserved, account.available);
}

// Required remaining margin/fee claim for an order at its limit price.
bool Claim(const model::OrderRequest& request,
           const model::InstrumentDefinition& instrument, int64_t quantity,
           int64_t& out) noexcept {
  int64_t fee;
  if (!Mul(Fee(instrument, request.position_effect), quantity, fee)) {
    return false;
  }
  if (request.position_effect != PositionEffect::kOpen) {
    out = fee;
    return true;
  }
  int64_t notional;
  int64_t margin;
  return Notional(instrument, request.limit_price, quantity, notional) &&
         Margin(notional, instrument.margin_rate_ppm, margin) &&
         Add(margin, fee, out);
}

std::optional<size_t> Find(
    const std::array<PositionRecord, market::kMaxMarketDataInstruments>&
        positions,
    size_t count, model::InstrumentId id) noexcept {
  for (size_t i = 0; i != count; ++i) {
    if (positions[i].position.instrument_id == id) {
      return i;
    }
  }
  return {};
}
model::OrderRequest Request(const model::OrderSnapshot& order) noexcept {
  return {order.instrument_id, order.side, order.limit_price,
          order.original_qty, order.position_effect};
}
}  // namespace

struct FuturesLedger::Storage {
  model::FuturesAccountSnapshot account;
  std::array<PositionRecord, market::kMaxMarketDataInstruments> positions{};
  std::array<OpeningLot, kMaxOpeningLots> lots{};
  size_t position_count = 0;
  size_t lot_count = 0;
};

FuturesLedger::FuturesLedger() noexcept = default;

FuturesLedger::~FuturesLedger() noexcept { Reset(); }

ErrorCode FuturesLedger::Fail(ErrorCode code, const char* detail) noexcept {
  ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_), "{}", detail);
  return code;
}
void FuturesLedger::Reset() noexcept {
  storage_.reset();
  instruments_ = nullptr;
  error_detail_[0] = '\0';
}

ErrorCode FuturesLedger::Configure(
    const TradingAccountSnapshot& account,
    const market::InstrumentScope* market_data) noexcept {
  if (storage_ != nullptr) {
    return Fail(ErrorCode::kInvalidState, "already configured");
  }
  if (account.type != model::AccountType::kFutures ||
      account.cash_balance < 0 || account.position_count != 0 ||
      account.futures_position_count > market::kMaxMarketDataInstruments) {
    return Fail(ErrorCode::kInvalidArgument,
                "invalid futures bootstrap account");
  }
  auto storage = std::unique_ptr<Storage>(new (std::nothrow) Storage);
  if (storage == nullptr) {
    return Fail(ErrorCode::kResourceExhausted, "cannot allocate FIFO ledger");
  }
  storage->account.balance = account.cash_balance;
  storage->account.equity = account.cash_balance;
  storage->account.available = account.cash_balance;
  for (size_t i = 0; i != account.futures_position_count; ++i) {
    const auto& initial = account.futures_positions[i];
    auto& position = storage->positions[i];
    bool valid = !Find(storage->positions, i, initial.instrument_id) &&
                 NewPosition(*market_data, initial.instrument_id, position) &&
                 initial.reference_price > 0 &&
                 initial.reference_price % position.instrument.price_tick == 0;
    const std::array<const FuturesInitialBucket*, 4> buckets = {
        &initial.long_today, &initial.long_yesterday, &initial.short_today,
        &initial.short_yesterday};
    position.bid = position.ask = initial.reference_price;
    for (size_t b = 0; valid && b != buckets.size(); ++b) {
      const auto& source = *buckets[b];
      auto& destination = Bucket(position.position, b);
      valid = source.quantity >= 0 &&
              ((source.quantity == 0 && source.open_price == 0) ||
               (source.quantity > 0 && source.open_price > 0 &&
                source.quantity % position.instrument.quantity_step == 0 &&
                source.open_price % position.instrument.price_tick == 0));
      if (!valid || source.quantity == 0) {
        continue;
      }
      int64_t unit;
      valid = Notional(position.instrument, source.open_price, 1, unit) &&
              Mul(unit, source.quantity, destination.open_notional);
      if (!valid) {
        break;
      }
      destination.quantity = source.quantity;
      const auto index = static_cast<uint32_t>(storage->lot_count++);
      storage->lots[index] = {source.quantity, unit, kNoLot};
      position.queues[b] = {index, index};
    }
    const model::FuturesPositionSnapshot empty;
    if (!valid || !Revalue(position) ||
        !RevalueAccount(storage->account, empty, position.position)) {
      return Fail(ErrorCode::kInvalidArgument,
                  "invalid or overflowing initial futures position");
    }
    ++storage->position_count;
  }
  storage_ = std::move(storage);
  instruments_ = market_data;
  error_detail_[0] = '\0';
  return ErrorCode::kOk;
}

ErrorCode FuturesLedger::Reserve(const model::OrderRequest& request,
                                 FuturesReservation& out) noexcept {
  if (storage_ == nullptr) {
    return Fail(ErrorCode::kInvalidState, "futures account disabled");
  }
  auto& s = *storage_;
  auto index = Find(s.positions, s.position_count, request.instrument_id);
  PositionRecord candidate;
  if (index) {
    candidate = s.positions[*index];
  } else if (!NewPosition(*instruments_, request.instrument_id, candidate)) {
    return Fail(ErrorCode::kInvalidArgument,
                "unknown or invalid futures instrument");
  }
  const auto& instrument = candidate.instrument;
  if (!ValidEffect(request.position_effect) ||
      (request.side != model::OrderSide::kBuy &&
       request.side != model::OrderSide::kSell) ||
      request.limit_price <= 0 || request.quantity <= 0 ||
      request.limit_price % instrument.price_tick != 0 ||
      request.quantity % instrument.quantity_step != 0) {
    return Fail(ErrorCode::kInvalidArgument,
                "invalid futures order specification or effect");
  }
  if (!index && s.position_count == s.positions.size()) {
    return Fail(ErrorCode::kResourceExhausted,
                "futures position capacity exhausted");
  }
  FuturesReservation reservation{request.quantity, 0};
  if (request.position_effect == PositionEffect::kOpen &&
      s.lot_count == s.lots.size()) {
    return Fail(ErrorCode::kResourceExhausted,
                "opening FIFO capacity exhausted");
  }
  if (!Claim(request, instrument, request.quantity, reservation.cash)) {
    return Fail(ErrorCode::kInvalidArgument, "futures reservation overflows");
  }
  if (request.position_effect == PositionEffect::kOpen) {
    if (s.account.available < reservation.cash) {
      return Fail(ErrorCode::kResourceExhausted,
                  "insufficient opening margin and fees");
    }
  } else {
    auto& bucket = Bucket(candidate.position,
                          OrderBucket(request.side, request.position_effect));
    if (request.quantity > bucket.quantity - bucket.frozen_qty) {
      return Fail(ErrorCode::kResourceExhausted,
                  "insufficient unfrozen closing inventory");
    }
    bucket.frozen_qty += request.quantity;
  }
  auto account = s.account;
  if (!Add(account.reserved, reservation.cash, account.reserved) ||
      !Sub(account.available, reservation.cash, account.available)) {
    return Fail(ErrorCode::kInvalidArgument,
                "futures reserved account amount overflows");
  }
  if (!index) {
    index = s.position_count;
    ++s.position_count;
  }
  s.positions[*index] = candidate;
  s.account = account;
  out = reservation;
  return ErrorCode::kOk;
}

ErrorCode FuturesLedger::Release(const model::OrderSnapshot& order,
                                 FuturesReservation& reservation) noexcept {
  if (storage_ == nullptr) {
    return Fail(ErrorCode::kInvalidState, "futures account disabled");
  }
  auto& s = *storage_;
  const auto index = Find(s.positions, s.position_count, order.instrument_id);
  if (!index || reservation.quantity < 0 || reservation.cash < 0 ||
      reservation.cash > s.account.reserved ||
      !ValidEffect(order.position_effect)) {
    return Fail(ErrorCode::kCorrupt, "invalid futures release claim");
  }
  auto candidate = s.positions[*index];
  int64_t expected_claim;
  if (reservation.quantity != order.leaves_qty ||
      !Claim(Request(order), candidate.instrument, reservation.quantity,
             expected_claim) ||
      expected_claim != reservation.cash) {
    return Fail(ErrorCode::kCorrupt,
                "futures release disagrees with order claim");
  }
  if (order.position_effect != PositionEffect::kOpen) {
    auto& bucket = Bucket(candidate.position,
                          OrderBucket(order.side, order.position_effect));
    if (reservation.quantity > bucket.frozen_qty) {
      return Fail(ErrorCode::kCorrupt, "invalid closing frozen claim");
    }
    bucket.frozen_qty -= reservation.quantity;
  }
  auto account = s.account;
  account.reserved -= reservation.cash;
  if (!Sub(account.equity, account.margin, account.available) ||
      !Sub(account.available, account.reserved, account.available)) {
    return Fail(ErrorCode::kCorrupt, "released account amount overflows");
  }
  s.positions[*index] = candidate;
  s.account = account;
  reservation = {};
  return ErrorCode::kOk;
}

ErrorCode FuturesLedger::ReadOpeningCost(size_t position_index,
                                         size_t bucket_index, int64_t quantity,
                                         int64_t& out) noexcept {
  const auto& s = *storage_;
  auto cursor = s.positions[position_index].queues[bucket_index].head;

  int64_t opening_cost = 0;
  size_t visited = 0;
  while (quantity != 0) {
    if (cursor >= s.lot_count || ++visited > s.lot_count ||
        s.lots[cursor].quantity <= 0) {
      return Fail(ErrorCode::kCorrupt, "closing FIFO is inconsistent");
    }
    const auto& lot = s.lots[cursor];
    const auto consumed = std::min(quantity, lot.quantity);
    int64_t cost;
    if (!Mul(lot.unit_notional, consumed, cost) ||
        !Add(opening_cost, cost, opening_cost)) {
      return Fail(ErrorCode::kCorrupt, "closing FIFO cost overflows");
    }
    quantity -= consumed;
    cursor = lot.next;
  }
  out = opening_cost;
  return ErrorCode::kOk;
}

ErrorCode FuturesLedger::ApplyFill(const model::OrderSnapshot& order,
                                   const model::Fill& fill,
                                   FuturesReservation& reservation) noexcept {
  if (storage_ == nullptr) {
    return Fail(ErrorCode::kInvalidState, "futures account disabled");
  }
  auto& s = *storage_;
  const auto index = Find(s.positions, s.position_count, order.instrument_id);
  if (!index || !ValidEffect(order.position_effect) || fill.quantity <= 0 ||
      fill.quantity > reservation.quantity ||
      reservation.quantity != order.leaves_qty ||
      fill.instrument_id != order.instrument_id || fill.side != order.side ||
      fill.position_effect != order.position_effect || fill.price <= 0) {
    return Fail(ErrorCode::kCorrupt,
                "invalid futures execution or reservation");
  }
  const auto& previous = s.positions[*index];
  auto candidate = previous;
  const auto& instrument = candidate.instrument;
  int64_t expected_claim;
  if (!Claim(Request(order), instrument, reservation.quantity,
             expected_claim) ||
      expected_claim != reservation.cash) {
    return Fail(ErrorCode::kCorrupt, "execution disagrees with order claim");
  }
  if (fill.price % instrument.price_tick != 0 ||
      fill.quantity % instrument.quantity_step != 0) {
    return Fail(ErrorCode::kCorrupt, "execution price or quantity is off step");
  }
  FuturesReservation remaining{reservation.quantity - fill.quantity, 0};
  if (!Claim(Request(order), instrument, remaining.quantity, remaining.cash) ||
      remaining.cash > reservation.cash ||
      reservation.cash > s.account.reserved) {
    return Fail(ErrorCode::kCorrupt, "remaining futures claim overflows");
  }
  int64_t actual_notional;
  int64_t unit_notional;
  int64_t fee;
  if (!Notional(instrument, fill.price, fill.quantity, actual_notional) ||
      !Notional(instrument, fill.price, 1, unit_notional) ||
      !Mul(Fee(instrument, order.position_effect), fill.quantity, fee)) {
    return Fail(ErrorCode::kCorrupt, "futures execution amount overflows");
  }
  const auto bucket_index = OrderBucket(order.side, order.position_effect);
  auto& bucket = Bucket(candidate.position, bucket_index);
  int64_t realized = 0;
  if (order.position_effect == PositionEffect::kOpen) {
    if (s.lot_count == s.lots.size()) {
      return Fail(ErrorCode::kResourceExhausted,
                  "opening FIFO capacity exhausted");
    }
    if (!Add(bucket.quantity, fill.quantity, bucket.quantity) ||
        !Add(bucket.open_notional, actual_notional, bucket.open_notional)) {
      return Fail(ErrorCode::kCorrupt, "opening position amount overflows");
    }
    if (candidate.bid == 0 || candidate.ask == 0) {
      candidate.bid = candidate.ask = fill.price;
    }
  } else {
    if (fill.quantity > bucket.frozen_qty || fill.quantity > bucket.quantity) {
      return Fail(ErrorCode::kCorrupt,
                  "closing execution exceeds frozen inventory");
    }
    int64_t opening_cost;
    const auto cost =
        ReadOpeningCost(*index, bucket_index, fill.quantity, opening_cost);
    if (cost != ErrorCode::kOk) {
      return cost;
    }
    if (opening_cost > bucket.open_notional ||
        !(order.side == model::OrderSide::kSell
              ? Sub(actual_notional, opening_cost, realized)
              : Sub(opening_cost, actual_notional, realized))) {
      return Fail(ErrorCode::kCorrupt, "closing realized PnL overflows");
    }
    bucket.quantity -= fill.quantity;
    bucket.frozen_qty -= fill.quantity;
    bucket.open_notional -= opening_cost;
  }
  auto account = s.account;
  account.reserved -= reservation.cash - remaining.cash;
  if (!Add(account.commission, fee, account.commission) ||
      !Add(account.realized_pnl, realized, account.realized_pnl) ||
      !Add(account.balance, realized, account.balance) ||
      !Sub(account.balance, fee, account.balance) || !Revalue(candidate) ||
      !RevalueAccount(account, previous.position, candidate.position)) {
    return Fail(ErrorCode::kCorrupt, "execution account valuation overflows");
  }
  // No operation below can fail: all account, position and FIFO effects were
  // checked against immutable old state before this commit boundary.
  auto& queue = candidate.queues[bucket_index];
  if (order.position_effect == PositionEffect::kOpen) {
    const auto added = static_cast<uint32_t>(s.lot_count++);
    s.lots[added] = {fill.quantity, unit_notional, kNoLot};
    if (queue.tail == kNoLot) {
      queue.head = added;
    } else {
      s.lots[queue.tail].next = added;
    }
    queue.tail = added;
  } else {
    auto quantity = fill.quantity;
    while (quantity != 0) {
      auto& lot = s.lots[queue.head];
      const auto consumed = std::min(quantity, lot.quantity);
      quantity -= consumed;
      lot.quantity -= consumed;
      if (lot.quantity == 0) {
        queue.head = lot.next;
      }
    }
    if (queue.head == kNoLot) {
      queue.tail = kNoLot;
    }
  }
  s.positions[*index] = candidate;
  s.account = account;
  reservation = remaining;
  return ErrorCode::kOk;
}

ErrorCode FuturesLedger::ApplyQuote(const model::QuoteTick& tick) noexcept {
  if (storage_ == nullptr) {
    return Fail(ErrorCode::kInvalidState, "futures account disabled");
  }
  auto& s = *storage_;
  auto index = Find(s.positions, s.position_count, tick.instrument_id);
  PositionRecord candidate;
  if (index) {
    candidate = s.positions[*index];
  } else if (!NewPosition(*instruments_, tick.instrument_id, candidate)) {
    return Fail(ErrorCode::kInvalidArgument,
                "unknown or non-futures marked instrument");
  }
  if (!model::HasQuoteFlag(tick.flags, model::QuoteFlag::kBidValid) ||
      !model::HasQuoteFlag(tick.flags, model::QuoteFlag::kAskValid) ||
      tick.bid_price <= 0 || tick.ask_price <= 0) {
    return ErrorCode::kOk;
  }
  if (!index && s.position_count == s.positions.size()) {
    return Fail(ErrorCode::kResourceExhausted,
                "futures mark position capacity exhausted");
  }
  const auto before = candidate.position;
  candidate.bid = tick.bid_price;
  candidate.ask = tick.ask_price;
  auto account = s.account;
  if (!Revalue(candidate) ||
      !RevalueAccount(account, before, candidate.position)) {
    return Fail(ErrorCode::kCorrupt, "marked futures account amount overflows");
  }
  if (!index) {
    index = s.position_count++;
  }
  s.positions[*index] = candidate;
  s.account = account;
  return ErrorCode::kOk;
}

bool FuturesLedger::TryGetAccount(
    model::FuturesAccountSnapshot& out) const noexcept {
  if (storage_ == nullptr) {
    return false;
  }
  out = storage_->account;
  return true;
}
bool FuturesLedger::TryGetPosition(
    model::InstrumentId id,
    model::FuturesPositionSnapshot& out) const noexcept {
  if (storage_ == nullptr) {
    return false;
  }
  const auto index = Find(storage_->positions, storage_->position_count, id);
  if (index) {
    out = storage_->positions[*index].position;
  } else {
    model::InstrumentDefinition instrument;
    if (!instruments_->TryGetInstrument(id, instrument) ||
        instrument.type != model::InstrumentType::kFutures) {
      return false;
    }
    out = {};
    out.instrument_id = id;
  }
  return true;
}
}  // namespace yquant::trading
