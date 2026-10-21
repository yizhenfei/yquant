#include "strategyd/trading.h"

#include <array>
#include <cstdio>
#include <limits>
#include <new>

#include "strategyd/futures_ledger.h"
#include "strategyd/trading_adapter.h"

namespace yquant::strategyd {
namespace {
using base::ErrorCode;
using model::OrderState;
using model::OrderUpdateKind;
constexpr std::uint64_t kCancelRetryNs = 10000000;
constexpr std::size_t kFillLookupCapacity = 2 * kMaxTradingFills;
constexpr std::size_t kBrokerLookupCapacity = 2 * kMaxTradingOrders;

// Broker IDs are unique within this one configured session, independently of
// local client identities. The lookup retains indices until Reset.
std::size_t BrokerHash(const model::BrokerOrderId& broker) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : broker.view()) {
    hash = (hash ^ byte) * 1099511628211ULL;
  }
  return hash & (kBrokerLookupCapacity - 1);
}

// Execution identity is order-scoped; only retained fill indices occupy the
// fixed lookup table. Its load stays at or below one half.
std::size_t FillHash(model::ClientOrderId order,
                     const model::ExecutionId& execution) noexcept {
  std::uint64_t hash =
      (14695981039346656037ULL ^ order.value) * 1099511628211ULL;
  for (const unsigned char byte : execution.view()) {
    hash = (hash ^ byte) * 1099511628211ULL;
  }
  return hash & (kFillLookupCapacity - 1);
}

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

struct TradingImpl::Storage {
  struct OrderRecord {
    model::OrderSnapshot snapshot;
    bool new_attempted = false;
    bool cancel_attempted = false;
    bool local_cancel_pending = false;
    std::uint64_t retry_not_before_ns = 0;
    std::size_t position_index = 0;
    std::int64_t price_scale = 1;
    // Remaining buy micro-yuan or sell shares, respectively. Fills release
    // their matched portion; only definitive terminal events release the rest.
    std::int64_t reserved_cash = 0;
    std::int64_t reserved_qty = 0;
    FuturesReservation futures_reservation;
  };
  enum class RequestKind { kNew, kCancel };
  struct Request {
    RequestKind kind = RequestKind::kNew;
    std::size_t order_index = 0;
    model::CancelRequestId cancel_id;
  };
  std::array<OrderRecord, kMaxTradingOrders> orders = {};
  std::array<model::Fill, kMaxTradingFills> fills = {};
  std::array<std::uint16_t, kFillLookupCapacity> fill_lookup = {};
  std::array<std::uint16_t, kBrokerLookupCapacity> broker_lookup = {};
  std::array<Request, kMaxTradingRequests> requests = {};
  std::array<std::size_t, kMaxLocalOrderUpdates> local_updates = {};
  std::size_t order_count = 0;
  std::size_t fill_count = 0;
  std::size_t active_count = 0;
  std::size_t pending_cancel_count = 0;
  std::size_t request_head = 0;
  std::size_t request_count = 0;
  std::size_t local_head = 0;
  std::size_t local_count = 0;
  std::uint64_t last_cancel_id = 0;
  std::size_t scan_cursor = 0;
  bool scan_pending = false;
  bool rescan_required = false;
  std::optional<std::uint64_t> next_retry;
  model::CashSnapshot cash;
  std::array<model::PositionSnapshot, kMaxMarketDataInstruments> positions = {};
  std::size_t position_count = 0;
  model::AccountType type = model::AccountType::kCash;
  FuturesLedger futures;
};

TradingImpl::TradingImpl(const MonotonicClock& clock,
                         const MarketDataImpl& market_data) noexcept
    : clock_(clock), market_data_(market_data) {}
TradingImpl::~TradingImpl() noexcept { Reset(); }

base::Status TradingImpl::Configure(
    const TradingAccountSnapshot& account) noexcept {
  if (storage_ != nullptr || phase_ != CallbackPhase::kNone || stopping_) {
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
    const auto configured = storage->futures.Configure(account, market_data_);
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
      account.position_count > kMaxMarketDataInstruments) {
    return {Fail(ErrorCode::kInvalidArgument,
                 "invalid initial cash or position count"),
            {}};
  }
  for (std::size_t index = 0; index < account.position_count; ++index) {
    const auto& position = account.positions[index];
    model::InstrumentDefinition instrument;
    if (!market_data_.TryGetInstrument(position.instrument_id, instrument) ||
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

void TradingImpl::Reset() noexcept {
  delete storage_;
  storage_ = nullptr;
  running_ = false;
  stopping_ = false;
  phase_ = CallbackPhase::kNone;
  error_detail_[0] = '\0';
}

void TradingImpl::BeginStopping() noexcept {
  running_ = false;
  if (!stopping_) {
    stopping_ = true;
    if (storage_ != nullptr) {
      storage_->scan_cursor = 0;
      storage_->scan_pending = true;
      storage_->next_retry.reset();
    }
  }
}

bool TradingImpl::accepting_orders() const noexcept {
  const bool business_callback =
      phase_ == CallbackPhase::kTick || phase_ == CallbackPhase::kOrderUpdate ||
      phase_ == CallbackPhase::kFill || phase_ == CallbackPhase::kTimer;
  return storage_ != nullptr && running_ && !stopping_ && business_callback;
}

ErrorCode TradingImpl::Fail(ErrorCode code, const char* detail) noexcept {
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
  return code;
}

std::optional<std::size_t> TradingImpl::FindIndex(
    model::ClientOrderId id) const noexcept {
  if (storage_ == nullptr || id.value == 0 || id.value > storage_->order_count)
    return {};
  return static_cast<std::size_t>(id.value - 1);
}

std::optional<std::size_t> TradingImpl::FindPosition(
    model::InstrumentId id) const noexcept {
  if (storage_ == nullptr) return {};
  for (std::size_t index = 0; index < storage_->position_count; ++index) {
    if (storage_->positions[index].instrument_id == id) return index;
  }
  return {};
}

ErrorCode TradingImpl::SubmitOrder(const model::OrderRequest& request,
                                   model::ClientOrderId& out) noexcept {
  if (!running_ || stopping_ ||
      (phase_ != CallbackPhase::kTick &&
       phase_ != CallbackPhase::kOrderUpdate &&
       phase_ != CallbackPhase::kFill && phase_ != CallbackPhase::kTimer)) {
    return Fail(ErrorCode::kInvalidState,
                "orders require a running business callback");
  }
  if (storage_ == nullptr)
    return Fail(ErrorCode::kUnsupported, "no trading adapter configured");
  model::InstrumentDefinition instrument;
  if (!market_data_.TryGetInstrument(request.instrument_id, instrument) ||
      !ValidSide(request.side) || request.limit_price <= 0 ||
      request.quantity <= 0 || instrument.price_decimals > 6 ||
      instrument.price_tick <= 0 || instrument.quantity_step <= 0 ||
      request.limit_price % instrument.price_tick != 0 ||
      request.quantity % instrument.quantity_step != 0) {
    return Fail(ErrorCode::kInvalidArgument, "invalid limit-order request");
  }
  auto& s = *storage_;
  if (s.order_count == kMaxTradingOrders ||
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
      s.position_count == kMaxMarketDataInstruments) {
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
  order.client_order_id.value = index + 1;
  order.instrument_id = request.instrument_id;
  order.side = request.side;
  order.limit_price = request.limit_price;
  order.original_qty = request.quantity;
  order.leaves_qty = request.quantity;
  order.position_effect = request.position_effect;
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
  ++s.order_count;
  ++s.active_count;
  out = order.client_order_id;
  return ErrorCode::kOk;
}

ErrorCode TradingImpl::QueueCancel(std::size_t index) noexcept {
  auto& s = *storage_;
  if (s.request_count == kMaxTradingRequests ||
      s.last_cancel_id == std::numeric_limits<std::uint64_t>::max()) {
    return Fail(ErrorCode::kResourceExhausted,
                "cancel request capacity or ID exhausted");
  }
  const model::CancelRequestId id{s.last_cancel_id + 1};
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
  return ErrorCode::kOk;
}

ErrorCode TradingImpl::CancelOrder(model::ClientOrderId id) noexcept {
  if (!running_ || stopping_ ||
      (phase_ != CallbackPhase::kTick &&
       phase_ != CallbackPhase::kOrderUpdate &&
       phase_ != CallbackPhase::kFill && phase_ != CallbackPhase::kTimer)) {
    return Fail(ErrorCode::kInvalidState,
                "cancellation requires a running business callback");
  }
  if (storage_ == nullptr)
    return Fail(ErrorCode::kUnsupported, "no trading adapter configured");
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

bool TradingImpl::TryGetOrder(model::ClientOrderId id,
                              model::OrderSnapshot& out) const noexcept {
  const auto index = FindIndex(id);
  if (!index) return false;
  out = storage_->orders[*index].snapshot;
  return true;
}

bool TradingImpl::TryGetCash(model::CashSnapshot& out) const noexcept {
  if (storage_ == nullptr || storage_->type != model::AccountType::kCash)
    return false;
  out = storage_->cash;
  return true;
}

bool TradingImpl::TryGetPosition(model::InstrumentId id,
                                 model::PositionSnapshot& out) const noexcept {
  model::InstrumentDefinition instrument;
  if (storage_ == nullptr || storage_->type != model::AccountType::kCash ||
      !market_data_.TryGetInstrument(id, instrument) ||
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

bool TradingImpl::TryGetFuturesAccount(
    model::FuturesAccountSnapshot& out) const noexcept {
  return storage_ != nullptr &&
         storage_->type == model::AccountType::kFutures &&
         storage_->futures.TryGetAccount(out);
}

bool TradingImpl::TryGetFuturesPosition(
    model::InstrumentId id,
    model::FuturesPositionSnapshot& out) const noexcept {
  return storage_ != nullptr &&
         storage_->type == model::AccountType::kFutures &&
         storage_->futures.TryGetPosition(id, out);
}

ErrorCode TradingImpl::ApplyQuote(const model::QuoteTick& tick) noexcept {
  if (storage_ == nullptr || storage_->type == model::AccountType::kCash)
    return ErrorCode::kOk;
  const auto applied = storage_->futures.ApplyQuote(tick);
  return applied == ErrorCode::kOk
             ? applied
             : Fail(applied, storage_->futures.error_detail());
}

base::Status TradingImpl::ProcessRequests(TradingAdapter& adapter,
                                          std::uint64_t now,
                                          std::size_t limit) noexcept {
  if (phase_ != CallbackPhase::kNone) {
    return {
        Fail(ErrorCode::kInvalidState, "cannot send from a strategy callback"),
        {}};
  }
  if (storage_ == nullptr) return {};
  auto& s = *storage_;
  for (std::size_t count = 0; count < limit && s.request_count != 0; ++count) {
    const auto request = s.requests[s.request_head];
    auto& record = s.orders[request.order_index];
    if (request.kind == Storage::RequestKind::kNew && stopping_) {
      if (s.local_count == kMaxLocalOrderUpdates) break;
      // Reserving the local event and removing the send are one commit. The
      // snapshot stays PendingNew until the local event consumes event budget.
      s.local_updates[(s.local_head + s.local_count) % kMaxLocalOrderUpdates] =
          request.order_index;
      ++s.local_count;
      record.local_cancel_pending = true;
      s.request_head = (s.request_head + 1) % kMaxTradingRequests;
      --s.request_count;
      continue;
    }
    s.request_head = (s.request_head + 1) % kMaxTradingRequests;
    --s.request_count;
    base::Status result;
    if (request.kind == Storage::RequestKind::kNew) {
      record.new_attempted = true;
      const auto& order = record.snapshot;
      const model::OrderRequest submitted{order.instrument_id, order.side,
                                          order.limit_price, order.original_qty,
                                          order.position_effect};
      result = adapter.SendOrder(order.client_order_id, submitted, now);
    } else {
      record.cancel_attempted = true;
      result = adapter.SendCancel(record.snapshot.client_order_id,
                                  record.snapshot.broker_order_id,
                                  request.cancel_id, now);
    }
    if (!result.ok()) {
      (void)Fail(result.code, adapter.error_detail());
      return result;
    }
  }
  return {};
}

base::Status TradingImpl::ScheduleStopping(std::uint64_t now,
                                           std::size_t max_scan) noexcept {
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

std::optional<std::uint64_t> TradingImpl::next_deadline() const noexcept {
  if (storage_ == nullptr) return {};
  const auto& s = *storage_;
  if (s.request_count != 0 || s.local_count != 0 ||
      (stopping_ && s.scan_pending))
    return 0;
  return stopping_ ? s.next_retry : std::nullopt;
}

bool TradingImpl::has_local_updates() const noexcept {
  return storage_ != nullptr && storage_->local_count != 0;
}
bool TradingImpl::is_drained() const noexcept {
  return storage_ == nullptr ||
         (storage_->active_count == 0 && storage_->pending_cancel_count == 0 &&
          storage_->request_count == 0 && storage_->local_count == 0);
}
std::size_t TradingImpl::order_count() const noexcept {
  return storage_ != nullptr ? storage_->order_count : 0;
}

ErrorCode TradingImpl::ApplyLocalUpdate(TradingDispatch& out) noexcept {
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
  if (s.type == model::AccountType::kFutures) {
    const auto released =
        s.futures.Release(record.snapshot, record.futures_reservation);
    if (released != ErrorCode::kOk)
      return Fail(released, s.futures.error_detail());
  } else {
    auto cash = s.cash;
    auto position = s.positions[record.position_index];
    if (!ReleaseReservation(cash, position, record.reserved_cash,
                            record.reserved_qty)) {
      return Fail(ErrorCode::kCorrupt,
                  "invalid local cancellation reservation");
    }
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
  out = dispatch;
  return ErrorCode::kOk;
}

ErrorCode TradingImpl::ApplyEvent(const TradingEvent& event,
                                  TradingDispatch& out) noexcept {
  const auto index = FindIndex(event.client_order_id);
  if (!index) return Fail(ErrorCode::kCorrupt, "report for an unknown order");
  auto& s = *storage_;
  auto& record = s.orders[*index];
  const auto& old = record.snapshot;
  const bool is_fill = event.kind == OrderUpdateKind::kFill;
  const bool is_cancel = event.kind == OrderUpdateKind::kCanceled ||
                         event.kind == OrderUpdateKind::kCancelRejected;
  if (!ValidKind(event.kind) || event.report_sequence == 0 ||
      event.receive_time_ns > clock_.Now() || event.reason.size > 128 ||
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
        !market_data_.TryGetInstrument(fill.instrument_id, instrument) ||
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
    broker_slot = BrokerHash(event.broker_order_id);
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
      fill_slot = FillHash(event.client_order_id, event.fill.execution_id);
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
        const auto now = clock_.Now();
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
  out = dispatch;
  return ErrorCode::kOk;
}
}  // namespace yquant::strategyd
