#include "yquant/trading/trading_engine.h"

#include <array>
#include <cstdio>
#include <limits>
#include <new>

#include "trading/trading_storage.h"
#include "yquant/trading/futures_ledger.h"
#include "yquant/trading/trading_adapter.h"

namespace yquant::trading {
namespace {
using base::ErrorCode;
using model::OrderState;
using model::OrderUpdateKind;
// Two hex characters per byte of a 64-byte opaque ID, plus the terminator.
constexpr std::size_t kTradingIdHexSize = 129;

bool IsDue(std::optional<std::uint64_t> deadline, std::uint64_t now) noexcept {
  return deadline && *deadline <= now;
}

// Preserve opaque broker/execution identities in failure diagnostics.
void CopyIdHex(std::string_view value,
               char (&out)[kTradingIdHexSize]) noexcept {
  constexpr char kHex[] = "0123456789abcdef";
  for (std::size_t index = 0; index < value.size(); ++index) {
    const auto byte = static_cast<unsigned char>(value[index]);
    out[2 * index] = kHex[byte >> 4];
    out[2 * index + 1] = kHex[byte & 15];
  }
  out[2 * value.size()] = '\0';
}

constexpr std::uint64_t kCancelRetryNs = 10000000;

bool ValidSide(model::OrderSide side) noexcept {
  return side == model::OrderSide::kBuy || side == model::OrderSide::kSell;
}

// Non-fill reports may not smuggle a second, unprocessed execution payload.
bool EmptyFill(const model::Fill& fill) noexcept {
  return fill.client_order_id.value == 0 && fill.broker_order_id.empty() &&
         fill.execution_id.empty() && fill.instrument_id.value == 0 &&
         fill.side == model::OrderSide::kUnknown && fill.price == 0 &&
         fill.quantity == 0 && fill.event_time_ns == 0 &&
         fill.receive_time_ns == 0 && !fill.has_event_time &&
         fill.position_effect == model::PositionEffect::kNone;
}

bool ValidKind(OrderUpdateKind kind) noexcept {
  return kind == OrderUpdateKind::kAccepted ||
         kind == OrderUpdateKind::kRejected || kind == OrderUpdateKind::kFill ||
         kind == OrderUpdateKind::kCanceled ||
         kind == OrderUpdateKind::kCancelRejected;
}

// All supported prices convert exactly to micro-yuan; no floating point or
// rounding is involved. Failure leaves out unchanged.
bool CashAmount(std::int64_t price, std::int64_t quantity, std::int64_t scale,
                std::int64_t& out) noexcept {
  constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
  if (price <= 0 || quantity <= 0 || scale <= 0 ||
      price > kMaximum / quantity) {
    return false;
  }
  const auto product = price * quantity;
  if (product > kMaximum / scale) return false;
  out = product * scale;
  return true;
}

// Releases only an order's remaining reservations. Validate both ledgers
// before changing either output, including share availability additions.
bool ReleaseReservation(model::CashSnapshot& cash,
                        model::PositionSnapshot& position,
                        std::int64_t reserved_cash,
                        std::int64_t reserved_qty) noexcept {
  if (reserved_cash < 0 || reserved_cash > cash.reserved ||
      cash.reserved > cash.balance || reserved_qty < 0 ||
      reserved_qty > position.reserved_qty || position.available_qty < 0 ||
      position.available_qty > position.total_qty ||
      reserved_qty > position.total_qty - position.available_qty) {
    return false;
  }
  cash.reserved -= reserved_cash;
  cash.available = cash.balance - cash.reserved;
  position.reserved_qty -= reserved_qty;
  position.available_qty += reserved_qty;
  return true;
}
}  // namespace

TradingEngine::TradingEngine(const base::MonotonicClock& clock,
                             const md::InstrumentScope& instruments,
                             TradingAdapter& adapter) noexcept
    : clock_(clock), instruments_(instruments), adapter_(adapter) {}
TradingEngine::~TradingEngine() noexcept { Reset(); }

base::Status TradingEngine::ConfigureAccount(
    const TradingAccountSnapshot& account) noexcept {
  if (storage_ != nullptr || accept_requests_ || stopping_) {
    return {Fail(ErrorCode::kInvalidState, "trading is already configured"),
            {}};
  }
  if (account.type == model::AccountType::kFutures) {
    auto* storage = new (std::nothrow) Storage;
    if (storage == nullptr) {
      return {Fail(ErrorCode::kResourceExhausted,
                   "cannot allocate fixed trading storage"),
              {}};
    }
    const auto configured = storage->futures.Configure(account, instruments_);
    if (configured != ErrorCode::kOk) {
      (void)Fail(configured, storage->futures.error_detail());
      delete storage;
      return {configured, {}};
    }
    storage->type = model::AccountType::kFutures;
    storage_ = storage;
    error_detail_[0] = '\0';
    return {};
  }
  if (account.type != model::AccountType::kCash ||
      account.futures_position_count != 0) {
    return {Fail(ErrorCode::kInvalidArgument,
                 "cash account cannot contain futures inventory"),
            {}};
  }
  if (account.cash_balance < 0 ||
      account.position_count > md::kMaxMarketDataInstruments) {
    return {Fail(ErrorCode::kInvalidArgument,
                 "invalid initial cash or position count"),
            {}};
  }
  for (std::size_t index = 0; index < account.position_count; ++index) {
    const auto& position = account.positions[index];
    model::InstrumentDefinition instrument;
    if (!instruments_.TryGetInstrument(position.instrument_id, instrument) ||
        instrument.type != model::InstrumentType::kEquity ||
        position.total_qty < 0 || position.available_qty < 0 ||
        position.available_qty > position.total_qty ||
        position.reserved_qty != 0 || instrument.quantity_step <= 0 ||
        position.total_qty % instrument.quantity_step != 0 ||
        position.available_qty % instrument.quantity_step != 0) {
      return {Fail(ErrorCode::kInvalidArgument,
                   "invalid initial position or unknown instrument"),
              {}};
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (account.positions[previous].instrument_id == position.instrument_id) {
        return {Fail(ErrorCode::kInvalidArgument, "duplicate initial position"),
                {}};
      }
    }
  }
  auto* storage = new (std::nothrow) Storage;
  if (storage == nullptr) {
    return {Fail(ErrorCode::kResourceExhausted,
                 "cannot allocate fixed trading storage"),
            {}};
  }
  storage->cash = {account.cash_balance, 0, account.cash_balance};
  storage->position_count = account.position_count;
  for (std::size_t index = 0; index < account.position_count; ++index) {
    storage->positions[index] = account.positions[index];
  }
  storage_ = storage;
  error_detail_[0] = '\0';
  return {};
}

void TradingEngine::Reset() noexcept {
  (void)Stop();
  delete storage_;
  storage_ = nullptr;
  sink_ = nullptr;
  replaying_ = false;
  invoking_sink_ = false;
  replay_now_ = 0;
  accept_requests_ = false;
  stopping_ = false;
  prefer_local_ = true;
  error_stage_ = "";
  error_detail_[0] = '\0';
}

void TradingEngine::BeginStopping() noexcept {
  accept_requests_ = false;
  if (!stopping_) {
    stopping_ = true;
    if (storage_ != nullptr) {
      storage_->scan_cursor = 0;
      storage_->scan_pending = true;
      storage_->next_retry.reset();
    }
  }
}

bool TradingEngine::accepting_orders() const noexcept {
  return prepared_ && storage_ != nullptr && accept_requests_ && !stopping_;
}

std::uint64_t TradingEngine::mutation_revision() const noexcept {
  return storage_ != nullptr ? storage_->mutation_revision : 0;
}

std::uint64_t TradingEngine::next_order_id() const noexcept {
  return storage_ != nullptr ? storage_->next_order_id : 1;
}

std::uint64_t TradingEngine::next_cancel_id() const noexcept {
  if (storage_ == nullptr) return 1;
  return storage_->last_cancel_id != UINT64_MAX ? storage_->last_cancel_id + 1
                                                : UINT64_MAX;
}

bool TradingEngine::TryGetOrderAt(std::size_t index,
                                  model::OrderSnapshot& out) const noexcept {
  if (storage_ == nullptr || index >= storage_->order_count) return false;
  out = storage_->orders[index].snapshot;
  return true;
}

std::size_t TradingEngine::fill_count() const noexcept {
  return storage_ != nullptr ? storage_->fill_count : 0;
}

bool TradingEngine::TryGetFillAt(std::size_t index,
                                 model::Fill& out) const noexcept {
  if (storage_ == nullptr || index >= storage_->fill_count) return false;
  out = storage_->fills[index];
  return true;
}

base::Status TradingEngine::SetNextOrderIdAtLeast(std::uint64_t id) noexcept {
  if (storage_ == nullptr || accept_requests_ || id == 0) {
    return {Fail(ErrorCode::kInvalidState, "invalid cold order-ID reservation"),
            {}};
  }
  if (id > storage_->next_order_id) storage_->next_order_id = id;
  return {};
}

base::Status TradingEngine::SetNextCancelIdAtLeast(std::uint64_t id) noexcept {
  if (storage_ == nullptr || accept_requests_ || id == 0) {
    return {
        Fail(ErrorCode::kInvalidState, "invalid cold cancel-ID reservation"),
        {}};
  }
  if (id - 1 > storage_->last_cancel_id) storage_->last_cancel_id = id - 1;
  return {};
}

base::Status TradingEngine::SetMutationSink(
    TradingMutationSink* sink) noexcept {
  if (accept_requests_ || replaying_ || invoking_sink_) {
    return {Fail(ErrorCode::kInvalidState,
                 "cannot replace the journal during ledger callbacks"),
            {}};
  }
  if (sink != nullptr) {
    if (storage_ == nullptr) {
      return {Fail(ErrorCode::kInvalidState,
                   "journal requires a configured cash ledger"),
              {}};
    }
    if (storage_->type != model::AccountType::kCash) {
      return {Fail(ErrorCode::kUnsupported,
                   "futures ledger persistence is unsupported"),
              {}};
    }
  }
  sink_ = sink;
  return {};
}

ErrorCode TradingEngine::BeforeCommit(TradingMutation& fact) noexcept {
  // Futures execution has no journal sink; its resource transaction is already
  // atomic and must not encounter a fallible cash persistence step afterward.
  if (storage_->type != model::AccountType::kCash) return ErrorCode::kOk;
  if (storage_->mutation_revision == UINT64_MAX) {
    return Fail(ErrorCode::kResourceExhausted, "ledger revision exhausted");
  }
  fact.revision = storage_->mutation_revision + 1;
  if (sink_ != nullptr && !replaying_) {
    invoking_sink_ = true;
    const auto status = sink_->BeforeCommit(fact);
    invoking_sink_ = false;
    if (!status.ok())
      return Fail(status.code, "ledger journal rejected mutation");
  }
  return ErrorCode::kOk;
}

ErrorCode TradingEngine::Fail(ErrorCode code, const char* detail) noexcept {
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
  return code;
}

std::optional<std::size_t> TradingEngine::FindIndex(
    model::ClientOrderId id) const noexcept {
  if (storage_ == nullptr || id.value == 0) return {};
  auto slot = TradingOrderHash(id);
  while (storage_->order_lookup[slot] != 0) {
    const auto index = storage_->order_lookup[slot] - 1;
    if (storage_->orders[index].snapshot.client_order_id == id) return index;
    slot = (slot + 1) & (kOrderLookupCapacity - 1);
  }
  return {};
}

std::optional<std::size_t> TradingEngine::FindPosition(
    model::InstrumentId id) const noexcept {
  if (storage_ == nullptr) return {};
  for (std::size_t index = 0; index < storage_->position_count; ++index) {
    if (storage_->positions[index].instrument_id == id) return index;
  }
  return {};
}

ErrorCode TradingEngine::SubmitOrder(const model::OrderRequest& request,
                                     model::ClientOrderId& out) noexcept {
  if (!accepting_orders()) {
    return Fail(ErrorCode::kInvalidState, "trading requests are not enabled");
  }
  model::InstrumentDefinition instrument;
  if (!instruments_.TryGetInstrument(request.instrument_id, instrument) ||
      !ValidSide(request.side) || request.limit_price <= 0 ||
      request.quantity <= 0 || instrument.price_decimals > 6 ||
      instrument.price_tick <= 0 || instrument.quantity_step <= 0 ||
      request.limit_price % instrument.price_tick != 0 ||
      request.quantity % instrument.quantity_step != 0) {
    return Fail(ErrorCode::kInvalidArgument, "invalid limit-order request");
  }
  auto& s = *storage_;
  if (s.order_count == kMaxTradingOrders ||
      s.next_order_id == std::numeric_limits<std::uint64_t>::max() ||
      s.request_count == kMaxTradingRequests) {
    return Fail(ErrorCode::kResourceExhausted,
                "order or request capacity exhausted");
  }
  const bool futures = s.type == model::AccountType::kFutures;
  if ((futures && instrument.type != model::InstrumentType::kFutures) ||
      (!futures && (instrument.type != model::InstrumentType::kEquity ||
                    request.position_effect != model::PositionEffect::kNone))) {
    return Fail(ErrorCode::kInvalidArgument,
                "instrument or position effect disagrees with account type");
  }
  constexpr std::array<std::int64_t, 7> kPriceScales = {
      1000000, 100000, 10000, 1000, 100, 10, 1};
  const auto scale = kPriceScales[instrument.price_decimals];
  std::int64_t notional = 0;
  if (!futures &&
      !CashAmount(request.limit_price, request.quantity, scale, notional)) {
    return Fail(ErrorCode::kInvalidArgument,
                "limit-order cash amount overflows");
  }
  const auto existing_position = FindPosition(request.instrument_id);
  if (!futures && !existing_position &&
      s.position_count == md::kMaxMarketDataInstruments) {
    return Fail(ErrorCode::kResourceExhausted, "position capacity exhausted");
  }
  model::PositionSnapshot position;
  position.instrument_id = request.instrument_id;
  if (existing_position) position = s.positions[*existing_position];
  auto cash = s.cash;
  FuturesReservation futures_reservation;
  if (futures) {
    // All fallible order/request checks precede the atomic account claim.
    // After it succeeds, the remaining order publication cannot fail.
    const auto reserved = s.futures.Reserve(request, futures_reservation);
    if (reserved != ErrorCode::kOk)
      return Fail(reserved, s.futures.error_detail());
  } else if (request.side == model::OrderSide::kBuy) {
    if (notional > cash.available) {
      return Fail(ErrorCode::kResourceExhausted, "insufficient available cash");
    }
    // notional <= available and reserved + available == balance <= INT64_MAX.
    cash.reserved += notional;
    cash.available -= notional;
  } else {
    if (request.quantity > position.available_qty) {
      return Fail(ErrorCode::kResourceExhausted,
                  "insufficient sellable shares");
    }
    position.available_qty -= request.quantity;
    position.reserved_qty += request.quantity;
  }
  const auto index = s.order_count;
  model::OrderSnapshot order;
  order.client_order_id.value = s.next_order_id;
  order.instrument_id = request.instrument_id;
  order.side = request.side;
  order.limit_price = request.limit_price;
  order.original_qty = request.quantity;
  order.leaves_qty = request.quantity;
  order.position_effect = request.position_effect;
  TradingMutation fact;
  fact.now = replaying_ ? replay_now_ : clock_.Now();
  fact.kind = TradingMutationKind::kNewOrder;
  fact.order_id = order.client_order_id;
  fact.request = request;
  const auto journal = BeforeCommit(fact);
  if (journal != ErrorCode::kOk) return journal;
  auto& record = s.orders[index];
  record.snapshot = order;
  record.position_index =
      existing_position ? *existing_position : s.position_count;
  record.price_scale = scale;
  record.reserved_cash = request.side == model::OrderSide::kBuy ? notional : 0;
  record.reserved_qty = !futures && request.side == model::OrderSide::kSell
                            ? request.quantity
                            : 0;
  record.futures_reservation = futures_reservation;
  if (!futures) {
    s.positions[record.position_index] = position;
    if (!existing_position) ++s.position_count;
    s.cash = cash;
  }
  s.requests[(s.request_head + s.request_count) % kMaxTradingRequests] = {
      Storage::RequestKind::kNew, index, {}};
  ++s.request_count;
  auto order_slot = TradingOrderHash(order.client_order_id);
  while (s.order_lookup[order_slot] != 0)
    order_slot = (order_slot + 1) & (kOrderLookupCapacity - 1);
  s.order_lookup[order_slot] = static_cast<std::uint16_t>(index + 1);
  ++s.order_count;
  ++s.active_count;
  ++s.next_order_id;
  if (s.type == model::AccountType::kCash) ++s.mutation_revision;
  out = order.client_order_id;
  return ErrorCode::kOk;
}

ErrorCode TradingEngine::QueueCancel(std::size_t index) noexcept {
  auto& s = *storage_;
  if (s.request_count == kMaxTradingRequests ||
      s.last_cancel_id == std::numeric_limits<std::uint64_t>::max()) {
    return Fail(ErrorCode::kResourceExhausted,
                "cancel request capacity or ID exhausted");
  }
  const model::CancelRequestId id{s.last_cancel_id + 1};
  TradingMutation fact;
  fact.now = replaying_ ? replay_now_ : clock_.Now();
  fact.kind = TradingMutationKind::kCancel;
  fact.order_id = s.orders[index].snapshot.client_order_id;
  fact.cancel_id = id;
  const auto journal = BeforeCommit(fact);
  if (journal != ErrorCode::kOk) return journal;
  auto& record = s.orders[index];
  s.requests[(s.request_head + s.request_count) % kMaxTradingRequests] = {
      Storage::RequestKind::kCancel, index, id};
  ++s.request_count;
  s.last_cancel_id = id.value;
  record.snapshot.cancel_request_id = id;
  record.snapshot.cancel_pending = true;
  record.cancel_attempted = false;
  record.retry_not_before_ns = 0;
  ++s.pending_cancel_count;
  if (s.type == model::AccountType::kCash) ++s.mutation_revision;
  return ErrorCode::kOk;
}

ErrorCode TradingEngine::CancelOrder(model::ClientOrderId id) noexcept {
  if (!accepting_orders()) {
    return Fail(ErrorCode::kInvalidState, "trading requests are not enabled");
  }
  const auto index = FindIndex(id);
  if (!index)
    return Fail(ErrorCode::kInvalidArgument, "unknown client order ID");
  const auto& order = storage_->orders[*index].snapshot;
  if (order.state != OrderState::kWorking || order.cancel_pending) {
    return Fail(ErrorCode::kInvalidState,
                "order is not cancellable or has an outstanding cancel");
  }
  return QueueCancel(*index);
}

bool TradingEngine::TryGetOrder(model::ClientOrderId id,
                                model::OrderSnapshot& out) const noexcept {
  const auto index = FindIndex(id);
  if (!index) return false;
  out = storage_->orders[*index].snapshot;
  return true;
}

bool TradingEngine::TryGetCash(model::CashSnapshot& out) const noexcept {
  if (storage_ == nullptr || storage_->type != model::AccountType::kCash)
    return false;
  out = storage_->cash;
  return true;
}

bool TradingEngine::TryGetPosition(
    model::InstrumentId id, model::PositionSnapshot& out) const noexcept {
  model::InstrumentDefinition instrument;
  if (storage_ == nullptr || storage_->type != model::AccountType::kCash ||
      !instruments_.TryGetInstrument(id, instrument) ||
      instrument.type != model::InstrumentType::kEquity)
    return false;
  const auto index = FindPosition(id);
  if (index) {
    out = storage_->positions[*index];
  } else {
    out = {id, 0, 0, 0};
  }
  return true;
}

bool TradingEngine::TryGetFuturesAccount(
    model::FuturesAccountSnapshot& out) const noexcept {
  return storage_ != nullptr &&
         storage_->type == model::AccountType::kFutures &&
         storage_->futures.TryGetAccount(out);
}

bool TradingEngine::TryGetFuturesPosition(
    model::InstrumentId id,
    model::FuturesPositionSnapshot& out) const noexcept {
  return storage_ != nullptr &&
         storage_->type == model::AccountType::kFutures &&
         storage_->futures.TryGetPosition(id, out);
}

ErrorCode TradingEngine::ApplyQuote(const model::QuoteTick& tick) noexcept {
  if (storage_ == nullptr || storage_->type == model::AccountType::kCash)
    return ErrorCode::kOk;
  const auto applied = storage_->futures.ApplyQuote(tick);
  return applied == ErrorCode::kOk
             ? applied
             : Fail(applied, storage_->futures.error_detail());
}

base::Status TradingEngine::ProcessRequests(std::uint64_t now,
                                            std::size_t limit) noexcept {
  error_stage_ = "trading_send";
  if (!prepared_ || storage_ == nullptr || accept_requests_) {
    return {
        Fail(ErrorCode::kInvalidState,
             "sending requires an initialized runtime with requests closed"),
        {}};
  }
  auto& s = *storage_;
  for (std::size_t count = 0; count < limit && s.request_count != 0; ++count) {
    const auto request = s.requests[s.request_head];
    auto& record = s.orders[request.order_index];
    if (request.kind == Storage::RequestKind::kNew && stopping_) {
      if (s.local_count == kMaxLocalOrderUpdates) break;
      TradingMutation fact;
      fact.now = replaying_ ? replay_now_ : clock_.Now();
      fact.kind = TradingMutationKind::kScheduleLocalCancel;
      fact.order_id = record.snapshot.client_order_id;
      const auto journal = BeforeCommit(fact);
      if (journal != ErrorCode::kOk) return {journal, {}};
      // Reserving the local event and removing the send are one commit. The
      // snapshot stays PendingNew until the local event consumes event budget.
      s.local_updates[(s.local_head + s.local_count) % kMaxLocalOrderUpdates] =
          request.order_index;
      ++s.local_count;
      record.local_cancel_pending = true;
      s.request_head = (s.request_head + 1) % kMaxTradingRequests;
      --s.request_count;
      if (s.type == model::AccountType::kCash) ++s.mutation_revision;
      continue;
    }
    TradingMutation fact;
    fact.now = replaying_ ? replay_now_ : clock_.Now();
    fact.kind = request.kind == Storage::RequestKind::kNew
                    ? TradingMutationKind::kSendNew
                    : TradingMutationKind::kSendCancel;
    fact.order_id = record.snapshot.client_order_id;
    fact.cancel_id = request.cancel_id;
    const auto journal = BeforeCommit(fact);
    if (journal != ErrorCode::kOk) return {journal, {}};
    s.request_head = (s.request_head + 1) % kMaxTradingRequests;
    --s.request_count;
    if (s.type == model::AccountType::kCash) ++s.mutation_revision;
    base::Status result;
    if (request.kind == Storage::RequestKind::kNew) {
      record.new_attempted = true;
      const auto& order = record.snapshot;
      const model::OrderRequest submitted{order.instrument_id, order.side,
                                          order.limit_price, order.original_qty,
                                          order.position_effect};
      result = adapter_.SendOrder(order.client_order_id, submitted, now);
    } else {
      record.cancel_attempted = true;
      result = adapter_.SendCancel(record.snapshot.client_order_id,
                                   record.snapshot.broker_order_id,
                                   request.cancel_id, now);
    }
    if (!result.ok()) {
      (void)Fail(result.code, adapter_.error_detail());
      return result;
    }
  }
  return {};
}

base::Status TradingEngine::ScheduleStopping(std::uint64_t now,
                                             std::size_t max_scan) noexcept {
  error_stage_ = "trading_drain";
  if (!stopping_ || storage_ == nullptr) return {};
  auto& s = *storage_;
  if (!s.scan_pending) {
    // Only due retries need a new full pass; state transitions explicitly
    // invalidate completed scans when they create newly cancellable work.
    if (!s.next_retry || *s.next_retry > now) return {};
    s.scan_pending = true;
    s.scan_cursor = 0;
    s.next_retry.reset();
  }
  for (std::size_t scanned = 0;
       scanned < max_scan && s.scan_cursor < s.order_count; ++scanned) {
    auto& record = s.orders[s.scan_cursor];
    const auto& order = record.snapshot;
    if (order.state == OrderState::kWorking && !order.cancel_pending &&
        record.retry_not_before_ns <= now) {
      if (s.request_count == kMaxTradingRequests) return {};
      const auto result = QueueCancel(s.scan_cursor);
      if (result != ErrorCode::kOk) return {result, {}};
    } else if (order.state == OrderState::kWorking && !order.cancel_pending &&
               record.retry_not_before_ns > now &&
               (!s.next_retry || record.retry_not_before_ns < *s.next_retry)) {
      s.next_retry = record.retry_not_before_ns;
    }
    ++s.scan_cursor;
  }
  if (s.scan_cursor == s.order_count) {
    s.scan_pending = s.rescan_required;
    s.rescan_required = false;
    s.scan_cursor = 0;
    if (s.scan_pending) s.next_retry.reset();
  }
  return {};
}

std::optional<std::uint64_t> TradingEngine::next_deadline() const noexcept {
  if (storage_ == nullptr) return {};
  const auto& s = *storage_;
  if (s.request_count != 0 || s.local_count != 0 ||
      (stopping_ && s.scan_pending))
    return 0;
  auto deadline = stopping_ ? s.next_retry : std::nullopt;
  const auto adapter_deadline =
      prepared_ ? adapter_.next_deadline() : std::nullopt;
  if (adapter_deadline && (!deadline || *adapter_deadline < *deadline))
    deadline = adapter_deadline;
  return deadline;
}

bool TradingEngine::has_ready_event(std::uint64_t now) const noexcept {
  const auto deadline = prepared_ ? adapter_.next_deadline() : std::nullopt;
  return has_local_updates() || IsDue(deadline, now);
}

bool TradingEngine::has_local_updates() const noexcept {
  return storage_ != nullptr && storage_->local_count != 0;
}
bool TradingEngine::is_drained() const noexcept {
  return storage_ == nullptr ||
         (storage_->active_count == 0 && storage_->pending_cancel_count == 0 &&
          storage_->request_count == 0 && storage_->local_count == 0 &&
          (!prepared_ || adapter_.is_drained()));
}
std::size_t TradingEngine::order_count() const noexcept {
  return storage_ != nullptr ? storage_->order_count : 0;
}

ErrorCode TradingEngine::ApplyLocalUpdate(TradingDispatch& out) noexcept {
  error_stage_ = "trading_local_update";
  if (storage_ == nullptr || storage_->local_count == 0) {
    return Fail(ErrorCode::kInvalidState, "no local order update available");
  }
  auto& s = *storage_;
  auto& record = s.orders[s.local_updates[s.local_head]];
  if (record.new_attempted || !record.local_cancel_pending ||
      record.snapshot.state != OrderState::kPendingNew) {
    return Fail(ErrorCode::kCorrupt, "invalid pending local cancellation");
  }
  TradingDispatch dispatch;
  dispatch.notify_order = true;
  dispatch.update.kind = OrderUpdateKind::kCanceledBeforeSend;
  dispatch.update.order = record.snapshot;
  dispatch.update.order.state = OrderState::kCanceled;
  dispatch.update.order.leaves_qty = 0;
  dispatch.update.reason.Set(0, "canceled before sending during shutdown");
  auto cash = s.cash;
  auto position = s.positions[record.position_index];
  if (s.type == model::AccountType::kFutures) {
    const auto released =
        s.futures.Release(record.snapshot, record.futures_reservation);
    if (released != ErrorCode::kOk)
      return Fail(released, s.futures.error_detail());
  } else {
    if (!ReleaseReservation(cash, position, record.reserved_cash,
                            record.reserved_qty)) {
      return Fail(ErrorCode::kCorrupt,
                  "invalid local cancellation reservation");
    }
    TradingMutation fact;
    fact.now = replaying_ ? replay_now_ : clock_.Now();
    fact.kind = TradingMutationKind::kLocalCancel;
    fact.order_id = record.snapshot.client_order_id;
    const auto journal = BeforeCommit(fact);
    if (journal != ErrorCode::kOk) return journal;
    s.cash = cash;
    s.positions[record.position_index] = position;
  }
  record.reserved_cash = 0;
  record.reserved_qty = 0;
  record.snapshot = dispatch.update.order;
  record.local_cancel_pending = false;
  --s.active_count;
  s.local_head = (s.local_head + 1) % kMaxLocalOrderUpdates;
  --s.local_count;
  if (s.type == model::AccountType::kCash) ++s.mutation_revision;
  out = dispatch;
  return ErrorCode::kOk;
}

ErrorCode TradingEngine::ApplyEvent(const TradingEvent& event,
                                    TradingDispatch& out) noexcept {
  error_stage_ = "trading_event";
  const auto transaction_now = replaying_ ? replay_now_ : clock_.Now();
  const auto index = FindIndex(event.client_order_id);
  if (!index) return Fail(ErrorCode::kCorrupt, "report for an unknown order");
  auto& s = *storage_;
  auto& record = s.orders[*index];
  const auto& old = record.snapshot;
  const bool is_fill = event.kind == OrderUpdateKind::kFill;
  const bool is_cancel = event.kind == OrderUpdateKind::kCanceled ||
                         event.kind == OrderUpdateKind::kCancelRejected;
  if (!ValidKind(event.kind) || event.report_sequence == 0 ||
      event.receive_time_ns > transaction_now || event.reason.size > 128 ||
      event.reason.text[event.reason.size] != '\0' ||
      event.cumulative_filled_qty < 0 ||
      event.cumulative_filled_qty > old.original_qty ||
      (is_cancel != (event.cancel_request_id.value != 0)) ||
      (!is_fill && !EmptyFill(event.fill)) ||
      ((is_fill || event.kind == OrderUpdateKind::kAccepted) &&
       event.broker_order_id.empty())) {
    return Fail(ErrorCode::kCorrupt, "invalid report fields or timestamp");
  }
  if (is_fill) {
    const auto& fill = event.fill;
    model::InstrumentDefinition instrument;
    if (fill.client_order_id != old.client_order_id ||
        fill.broker_order_id != event.broker_order_id ||
        fill.execution_id.empty() || fill.instrument_id != old.instrument_id ||
        fill.side != old.side || fill.position_effect != old.position_effect ||
        fill.price <= 0 || fill.quantity <= 0 ||
        !instruments_.TryGetInstrument(fill.instrument_id, instrument) ||
        instrument.price_tick <= 0 || instrument.quantity_step <= 0 ||
        fill.price % instrument.price_tick != 0 ||
        fill.quantity % instrument.quantity_step != 0 ||
        fill.receive_time_ns != event.receive_time_ns ||
        (!fill.has_event_time && fill.event_time_ns != 0) ||
        (old.side == model::OrderSide::kBuy && fill.price > old.limit_price) ||
        (old.side == model::OrderSide::kSell && fill.price < old.limit_price)) {
      return Fail(ErrorCode::kCorrupt, "invalid execution fields");
    }
  }
  if (event.report_sequence <= old.report_sequence) {
    out = {};
    return ErrorCode::kOk;
  }
  if (!record.new_attempted || record.local_cancel_pending ||
      (old.state == OrderState::kCanceled && old.report_sequence == 0) ||
      event.report_sequence != old.report_sequence + 1 ||
      (!old.broker_order_id.empty() && !event.broker_order_id.empty() &&
       old.broker_order_id != event.broker_order_id)) {
    return Fail(ErrorCode::kCorrupt,
                "report sequence or broker identity mismatch");
  }
  auto next = old;
  const bool bind_broker =
      old.broker_order_id.empty() && !event.broker_order_id.empty();
  std::size_t broker_slot = 0;
  if (bind_broker) {
    broker_slot = TradingBrokerHash(event.broker_order_id);
    while (s.broker_lookup[broker_slot] != 0) {
      const auto bound_index = s.broker_lookup[broker_slot] - 1;
      if (s.orders[bound_index].snapshot.broker_order_id ==
          event.broker_order_id) {
        return Fail(ErrorCode::kCorrupt,
                    "broker order ID is already bound to another client order");
      }
      broker_slot = (broker_slot + 1) & (kBrokerLookupCapacity - 1);
    }
  }
  if (!event.broker_order_id.empty())
    next.broker_order_id = event.broker_order_id;
  next.report_sequence = event.report_sequence;
  next.receive_time_ns = event.receive_time_ns;
  TradingDispatch dispatch;
  dispatch.notify_order = true;
  dispatch.update.kind = event.kind;
  dispatch.update.cancel_request_id = event.cancel_request_id;
  dispatch.update.reason = event.reason;
  std::size_t fill_slot = 0;
  std::uint64_t retry = record.retry_not_before_ns;
  if (!is_fill && event.cumulative_filled_qty != old.cumulative_filled_qty) {
    return Fail(ErrorCode::kCorrupt,
                "non-fill cumulative quantity disagrees with ledger");
  }
  switch (event.kind) {
    case OrderUpdateKind::kAccepted:
      if (old.state == OrderState::kPendingNew)
        next.state = OrderState::kWorking;
      else if (old.state == OrderState::kWorking ||
               old.state == OrderState::kFilled ||
               old.state == OrderState::kCanceled)
        dispatch.notify_order = false;
      else
        return Fail(ErrorCode::kCorrupt,
                    "acceptance would reverse a terminal order");
      break;
    case OrderUpdateKind::kRejected:
      if (old.state != OrderState::kPendingNew ||
          old.cumulative_filled_qty != 0) {
        return Fail(ErrorCode::kCorrupt,
                    "new-order rejection after acceptance or fill");
      }
      next.state = OrderState::kRejected;
      next.leaves_qty = 0;
      break;
    case OrderUpdateKind::kFill: {
      if (model::IsTerminalOrderState(old.state) ||
          event.fill.quantity > old.original_qty - old.cumulative_filled_qty ||
          event.cumulative_filled_qty !=
              old.cumulative_filled_qty + event.fill.quantity) {
        return Fail(ErrorCode::kCorrupt,
                    "execution reverses terminal state or overfills order");
      }
      if (s.fill_count == kMaxTradingFills)
        return Fail(ErrorCode::kResourceExhausted,
                    "execution ledger capacity exhausted");
      fill_slot =
          TradingFillHash(event.client_order_id, event.fill.execution_id);
      while (s.fill_lookup[fill_slot] != 0) {
        const auto& previous = s.fills[s.fill_lookup[fill_slot] - 1];
        if (previous.client_order_id == event.client_order_id &&
            previous.execution_id == event.fill.execution_id) {
          return Fail(ErrorCode::kCorrupt,
                      "execution ID repeated with a new sequence");
        }
        fill_slot = (fill_slot + 1) & (kFillLookupCapacity - 1);
      }
      next.cumulative_filled_qty = event.cumulative_filled_qty;
      next.leaves_qty = old.original_qty - next.cumulative_filled_qty;
      next.state =
          next.leaves_qty == 0 ? OrderState::kFilled : OrderState::kWorking;
      dispatch.has_fill = true;
      dispatch.fill = event.fill;
      break;
    }
    case OrderUpdateKind::kCanceled:
    case OrderUpdateKind::kCancelRejected:
      if (!old.cancel_pending || !record.cancel_attempted ||
          event.cancel_request_id != old.cancel_request_id ||
          (old.state != OrderState::kWorking &&
           old.state != OrderState::kFilled)) {
        return Fail(ErrorCode::kCorrupt,
                    "cancel result does not match the pending attempt");
      }
      if (event.kind == OrderUpdateKind::kCanceled) {
        if (old.state != OrderState::kWorking)
          return Fail(ErrorCode::kCorrupt,
                      "cancellation would reverse a filled order");
        next.state = OrderState::kCanceled;
        next.leaves_qty = 0;
      } else if (old.state == OrderState::kWorking) {
        const auto now = transaction_now;
        if (now > UINT64_MAX - kCancelRetryNs)
          return Fail(ErrorCode::kResourceExhausted,
                      "cancel retry deadline overflows");
        retry = now + kCancelRetryNs;
      }
      next.cancel_pending = false;
      break;
    case OrderUpdateKind::kCanceledBeforeSend:
      return Fail(ErrorCode::kCorrupt,
                  "local cancellation received from adapter");
  }
  auto cash = s.cash;
  auto position = s.positions[record.position_index];
  auto reserved_cash = record.reserved_cash;
  auto reserved_qty = record.reserved_qty;
  if (s.type == model::AccountType::kFutures) {
    auto reservation = record.futures_reservation;
    ErrorCode applied = ErrorCode::kOk;
    if (dispatch.has_fill) {
      applied = s.futures.ApplyFill(old, dispatch.fill, reservation);
    } else if (event.kind == OrderUpdateKind::kRejected ||
               event.kind == OrderUpdateKind::kCanceled) {
      applied = s.futures.Release(old, reservation);
    }
    if (applied != ErrorCode::kOk)
      return Fail(applied, s.futures.error_detail());
    record.futures_reservation = reservation;
  } else if (dispatch.has_fill) {
    const auto& fill = dispatch.fill;
    std::int64_t actual_cash;
    if (!CashAmount(fill.price, fill.quantity, record.price_scale,
                    actual_cash)) {
      return Fail(ErrorCode::kCorrupt, "execution cash amount overflows");
    }
    if (old.side == model::OrderSide::kBuy) {
      std::int64_t limit_cash;
      if (!CashAmount(old.limit_price, fill.quantity, record.price_scale,
                      limit_cash) ||
          limit_cash > reserved_cash || actual_cash > cash.balance ||
          fill.quantity >
              std::numeric_limits<std::int64_t>::max() - position.total_qty ||
          !ReleaseReservation(cash, position, limit_cash, 0)) {
        return Fail(ErrorCode::kCorrupt,
                    "buy execution violates cash or holding bounds");
      }
      reserved_cash -= limit_cash;
      cash.balance -= actual_cash;
      cash.available = cash.balance - cash.reserved;
      // Buys become owned shares, but never replenish sellable bootstrap stock.
      position.total_qty += fill.quantity;
    } else {
      if (fill.quantity > reserved_qty ||
          fill.quantity > position.reserved_qty ||
          fill.quantity > position.total_qty ||
          actual_cash >
              std::numeric_limits<std::int64_t>::max() - cash.balance) {
        return Fail(ErrorCode::kCorrupt,
                    "sell execution violates cash or holding bounds");
      }
      reserved_qty -= fill.quantity;
      position.reserved_qty -= fill.quantity;
      position.total_qty -= fill.quantity;
      cash.balance += actual_cash;
      cash.available = cash.balance - cash.reserved;
    }
    if (next.state == OrderState::kFilled &&
        (reserved_cash != 0 || reserved_qty != 0)) {
      return Fail(ErrorCode::kCorrupt, "filled order retained a reservation");
    }
  } else if (event.kind == OrderUpdateKind::kRejected ||
             event.kind == OrderUpdateKind::kCanceled) {
    if (!ReleaseReservation(cash, position, reserved_cash, reserved_qty)) {
      return Fail(ErrorCode::kCorrupt,
                  "terminal order reservation is inconsistent");
    }
    reserved_cash = 0;
    reserved_qty = 0;
  }
  TradingMutation fact;
  fact.now = transaction_now;
  fact.kind = TradingMutationKind::kReport;
  fact.order_id = event.client_order_id;
  fact.event = event;
  const auto journal = BeforeCommit(fact);
  if (journal != ErrorCode::kOk) return journal;
  // All failure paths precede this transaction: no partial fill, sequence or
  // request completion can escape a rejected source event.
  if (dispatch.has_fill) {
    s.fills[s.fill_count] = dispatch.fill;
    s.fill_lookup[fill_slot] = static_cast<std::uint16_t>(s.fill_count + 1);
    ++s.fill_count;
  }
  if (bind_broker) {
    s.broker_lookup[broker_slot] = static_cast<std::uint16_t>(*index + 1);
  }
  if (!model::IsTerminalOrderState(old.state) &&
      model::IsTerminalOrderState(next.state))
    --s.active_count;
  if (old.cancel_pending && !next.cancel_pending) --s.pending_cancel_count;
  record.snapshot = next;
  if (s.type == model::AccountType::kCash) {
    s.cash = cash;
    s.positions[record.position_index] = position;
  }
  record.reserved_cash = reserved_cash;
  record.reserved_qty = reserved_qty;
  record.retry_not_before_ns = retry;
  if (is_cancel) record.cancel_attempted = false;
  if (stopping_) {
    if (!s.scan_pending) {
      s.scan_pending = true;
      s.scan_cursor = 0;
      s.next_retry.reset();
    } else if (*index < s.scan_cursor) {
      // Finish the current bounded pass instead of repeatedly restarting it
      // when reports arrive; revisit an already-scanned slot on the next pass.
      s.rescan_required = true;
    }
  }
  dispatch.update.order = next;
  if (s.type == model::AccountType::kCash) ++s.mutation_revision;
  out = dispatch;
  return ErrorCode::kOk;
}
base::Status TradingEngine::ReplayMutation(const TradingMutation& fact,
                                           TradingDispatch* out) noexcept {
  if (storage_ != nullptr && storage_->type != model::AccountType::kCash)
    return {Fail(ErrorCode::kUnsupported,
                 "futures ledger persistence is unsupported"),
            {}};
  if (storage_ == nullptr || accept_requests_ || replaying_ ||
      storage_->mutation_revision == UINT64_MAX ||
      fact.revision != storage_->mutation_revision + 1) {
    return {Fail(ErrorCode::kCorrupt, "invalid replay state or revision"), {}};
  }
  auto& s = *storage_;
  const auto index = FindIndex(fact.order_id);
  if (fact.kind != TradingMutationKind::kNewOrder && !index)
    return {Fail(ErrorCode::kCorrupt, "unknown replay order"), {}};
  replaying_ = true;
  replay_now_ = fact.now;
  ErrorCode result = ErrorCode::kOk;
  TradingDispatch dispatch;
  switch (fact.kind) {
    case TradingMutationKind::kNewOrder: {
      if (fact.order_id.value != s.next_order_id) {
        result = Fail(ErrorCode::kCorrupt, "replay order identity mismatch");
        break;
      }
      const auto saved_stopping = stopping_;
      accept_requests_ = true;
      stopping_ = false;
      model::ClientOrderId id;
      result = SubmitOrder(fact.request, id);
      accept_requests_ = false;
      stopping_ = saved_stopping;
      break;
    }
    case TradingMutationKind::kCancel:
      if (fact.cancel_id.value == 0 || s.last_cancel_id == UINT64_MAX ||
          fact.cancel_id.value != s.last_cancel_id + 1 ||
          s.orders[*index].snapshot.state != OrderState::kWorking ||
          s.orders[*index].snapshot.cancel_pending) {
        result = Fail(ErrorCode::kCorrupt, "invalid replay cancel");
      } else {
        result = QueueCancel(*index);
      }
      break;
    case TradingMutationKind::kSendNew:
    case TradingMutationKind::kSendCancel:
    case TradingMutationKind::kScheduleLocalCancel: {
      if (s.request_count == 0) {
        result = Fail(ErrorCode::kCorrupt, "missing replay request");
        break;
      }
      const auto request = s.requests[s.request_head];
      const bool is_new = request.kind == Storage::RequestKind::kNew;
      if (request.order_index != *index ||
          request.cancel_id != fact.cancel_id ||
          (is_new != (fact.kind != TradingMutationKind::kSendCancel)) ||
          (fact.kind == TradingMutationKind::kScheduleLocalCancel &&
           s.local_count == kMaxLocalOrderUpdates)) {
        result = Fail(ErrorCode::kCorrupt, "replay request mismatch");
        break;
      }
      auto& record = s.orders[*index];
      if (fact.kind == TradingMutationKind::kScheduleLocalCancel) {
        s.local_updates[(s.local_head + s.local_count) %
                        kMaxLocalOrderUpdates] = *index;
        ++s.local_count;
        record.local_cancel_pending = true;
      } else if (is_new) {
        record.new_attempted = true;
      } else {
        record.cancel_attempted = true;
      }
      s.request_head = (s.request_head + 1) % kMaxTradingRequests;
      --s.request_count;
      if (s.type == model::AccountType::kCash) ++s.mutation_revision;
      break;
    }
    case TradingMutationKind::kLocalCancel:
      if (s.local_count == 0 || s.local_updates[s.local_head] != *index)
        result =
            Fail(ErrorCode::kCorrupt, "replay local cancellation mismatch");
      else
        result = ApplyLocalUpdate(dispatch);
      break;
    case TradingMutationKind::kReport:
      if (fact.event.client_order_id != fact.order_id)
        result = Fail(ErrorCode::kCorrupt, "replay report identity mismatch");
      else
        result = ApplyEvent(fact.event, dispatch);
      break;
    default:
      result = Fail(ErrorCode::kCorrupt, "unknown replay mutation");
  }
  replaying_ = false;
  replay_now_ = 0;
  if (result == ErrorCode::kOk && s.mutation_revision != fact.revision)
    return {Fail(ErrorCode::kCorrupt, "replay fact did not commit"), {}};
  if (result == ErrorCode::kOk && out != nullptr) *out = dispatch;
  return {result, {}};
}

void TradingEngine::AbandonRecoveredRequests() noexcept {
  if (storage_ == nullptr || accept_requests_) return;
  auto& s = *storage_;
  for (std::size_t offset = 0; offset < s.request_count; ++offset) {
    const auto& request =
        s.requests[(s.request_head + offset) % kMaxTradingRequests];
    auto& record = s.orders[request.order_index];
    if (request.kind == Storage::RequestKind::kNew)
      record.new_attempted = true;
    else
      record.cancel_attempted = true;
  }
  s.request_count = 0;
  s.request_head = 0;
  s.scan_cursor = 0;
  s.scan_pending = false;
  s.rescan_required = false;
  s.next_retry.reset();
}
base::Status TradingEngine::Fail(base::Status status, const char* stage,
                                 const char* detail) noexcept {
  error_stage_ = stage;
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
  return status;
}

base::Status TradingEngine::Init(const TradingConfig& config,
                                 const md::MarketDataConfig&) noexcept {
  if (prepared_ || storage_ != nullptr || accept_requests_ || stopping_) {
    return Fail({base::ErrorCode::kInvalidState, {}}, "trading_prepare",
                "gateway engine already initialized");
  }
  if (config.adapter != TradingAdapterKind::kMock &&
      config.adapter != TradingAdapterKind::kSimulatedFutures) {
    return Fail({base::ErrorCode::kInvalidArgument, {}}, "trading_prepare",
                "gateway engine requires a broker configuration");
  }
  // Even failed Prepare may own resources; Stop must run exactly once.
  prepared_ = true;
  const auto prepared = adapter_.Prepare(config);
  if (!prepared.ok())
    return Fail(prepared, "trading_prepare", adapter_.error_detail());
  const auto started = adapter_.Start(clock_.Now());
  if (!started.ok())
    return Fail(started, "trading_start", adapter_.error_detail());
  TradingAccountSnapshot account;
  const auto snapshot = adapter_.ReadAccountSnapshot(account);
  if (!snapshot.ok())
    return Fail(snapshot, "trading_account", adapter_.error_detail());
  error_stage_ = "trading_account";
  return ConfigureAccount(account);
}

base::Status TradingEngine::Stop() noexcept {
  accept_requests_ = false;
  if (!prepared_) return {};
  prepared_ = false;
  const auto stopped = adapter_.Stop();
  if (!stopped.ok())
    return Fail(stopped, "trading_stop", adapter_.error_detail());
  return {};
}

base::Status TradingEngine::Poll(std::uint64_t now, TradingDispatch& out,
                                 bool& event) noexcept {
  error_stage_ = "trading_poll";
  if (!prepared_ || storage_ == nullptr || accept_requests_)
    return Fail({base::ErrorCode::kInvalidState, {}}, "trading_poll",
                "polling requires an initialized runtime with requests closed");
  TradingDispatch dispatch;
  if (has_local_updates() &&
      (!IsDue(adapter_.next_deadline(), now) || prefer_local_)) {
    prefer_local_ = false;
    const auto applied = ApplyLocalUpdate(dispatch);
    if (applied != base::ErrorCode::kOk) return {applied, {}};
  } else {
    prefer_local_ = true;
    TradingEvent input;
    const auto polled = adapter_.Poll(now, input);
    if (!polled.status.ok() || polled.state == TradingPollState::kError) {
      return Fail(polled.status.ok()
                      ? base::Status{base::ErrorCode::kCorrupt, {}}
                      : polled.status,
                  "trading_poll", adapter_.error_detail());
    }
    if (polled.state == TradingPollState::kIdle) {
      if (IsDue(adapter_.next_deadline(), now))
        return Fail({base::ErrorCode::kCorrupt, {}}, "trading_poll",
                    "idle trading adapter retained an already-due deadline");
      event = false;
      return {};
    }
    if (polled.state != TradingPollState::kEvent)
      return Fail({base::ErrorCode::kCorrupt, {}}, "trading_poll",
                  "adapter returned an unknown polling state");
    const auto applied = ApplyEvent(input, dispatch);
    if (applied != base::ErrorCode::kOk) {
      char broker[kTradingIdHexSize];
      char execution[kTradingIdHexSize];
      CopyIdHex(input.broker_order_id.view(), broker);
      CopyIdHex(input.fill.execution_id.view(), execution);
      error_stage_ = "trading_event";
      char detail[kErrorDetailSize];
      std::snprintf(
          detail, sizeof(detail),
          "client_order_id=%llu sequence=%llu kind=%u cumulative=%lld "
          "fill_price=%lld fill_qty=%lld cancel_request=%llu "
          "broker_id_hex=%s execution_id_hex=%s: %s",
          static_cast<unsigned long long>(input.client_order_id.value),
          static_cast<unsigned long long>(input.report_sequence),
          static_cast<unsigned>(input.kind),
          static_cast<long long>(input.cumulative_filled_qty),
          static_cast<long long>(input.fill.price),
          static_cast<long long>(input.fill.quantity),
          static_cast<unsigned long long>(input.cancel_request_id.value),
          broker, execution, error_detail_);
      (void)Fail({applied, {}}, "trading_event", detail);
      // Report even when the runner retains an earlier callback failure.
      std::fprintf(stderr, "yquant-trading: unapplied_trading_event %s\n",
                   error_detail_);
      return {applied, {}};
    }
  }
  out = dispatch;
  event = true;
  return {};
}

}  // namespace yquant::trading
