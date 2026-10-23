#include "yquant/trading/trading_engine.h"

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <cstdio>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

#include "trading/trading_storage.h"
#include "yquant/base/format.h"
#include "yquant/trading/futures_ledger.h"
#include "yquant/trading/trading_adapter.h"

namespace yquant::trading {
namespace {
using base::ErrorCode;
using model::OrderState;
using model::OrderUpdateKind;
// Two hex characters per byte of a 64-byte opaque ID, plus the terminator.
constexpr size_t kTradingIdHexSize = 129;

bool IsDue(std::optional<uint64_t> deadline, uint64_t now) noexcept {
  return deadline && *deadline <= now;
}

// Preserve opaque broker/execution identities in failure diagnostics.
void CopyIdHex(std::string_view value,
               char (&out)[kTradingIdHexSize]) noexcept {
  constexpr char kHex[] = "0123456789abcdef";
  for (size_t index = 0; index < value.size(); ++index) {
    const auto byte = static_cast<unsigned char>(value[index]);
    out[2 * index] = kHex[byte >> 4];
    out[2 * index + 1] = kHex[byte & 15];
  }
  out[2 * value.size()] = '\0';
}

// Converts owned domain effects into the engine's committed notification shape.
TradingDispatch DispatchOrder(const OrderApplyResult& result) noexcept {
  TradingDispatch dispatch;
  dispatch.update = result.update;
  for (size_t i = 0; i < result.action_count; ++i) {
    switch (result.actions[i]) {
      case OrderActionKind::kPublishOrderUpdate:
        dispatch.notify_order = true;
        break;
      case OrderActionKind::kApplyExecution:
        dispatch.has_fill = true;
        dispatch.fill = result.fill;
        break;
      case OrderActionKind::kSubmitOrder:
      case OrderActionKind::kCancelOrder:
      case OrderActionKind::kReleaseRemainingReservation:
        break;
    }
  }
  return dispatch;
}

constexpr uint64_t kCancelRetryNs = 10000000;

// Rejects unspecified directions before reserving account resources.
bool ValidSide(model::OrderSide side) noexcept {
  switch (side) {
    case model::OrderSide::kBuy:
    case model::OrderSide::kSell:
      return true;
    case model::OrderSide::kUnknown:
      return false;
  }
  return false;
}

// Selects legal closing intents for the account authority and venue.
bool SupportsPositionEffect(model::PositionEffect effect, bool broker_managed,
                            model::Venue venue) noexcept {
  const bool day_specific =
      venue == model::Venue::kShfe || venue == model::Venue::kIne;
  switch (effect) {
    case model::PositionEffect::kOpen:
      return true;
    case model::PositionEffect::kCloseToday:
    case model::PositionEffect::kCloseYesterday:
      return !broker_managed || day_specific;
    case model::PositionEffect::kClose:
      return broker_managed && !day_specific;
    case model::PositionEffect::kNone:
      return false;
  }
  return false;
}

// All supported prices convert exactly to micro-yuan; no floating point or
// rounding is involved. Failure leaves out unchanged.
bool CashAmount(int64_t price, int64_t quantity, int64_t scale,
                int64_t& out) noexcept {
  constexpr auto kMaximum = std::numeric_limits<int64_t>::max();
  if (price <= 0 || quantity <= 0 || scale <= 0 ||
      price > kMaximum / quantity) {
    return false;
  }
  const auto product = price * quantity;
  if (product > kMaximum / scale) {
    return false;
  }
  out = product * scale;
  return true;
}

// Releases only an order's remaining reservations. Validate both ledgers
// before changing either output, including share availability additions.
bool ReleaseReservation(model::CashSnapshot& cash,
                        model::PositionSnapshot& position,
                        int64_t reserved_cash, int64_t reserved_qty) noexcept {
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

TradingEngine::TradingEngine(const base::MonotonicClock* clock,
                             const md::InstrumentScope* instruments,
                             TradingAdapter* adapter) noexcept
    : clock_(*clock), instruments_(*instruments), adapter_(*adapter) {}
TradingEngine::~TradingEngine() noexcept { Reset(); }

base::Status TradingEngine::ConfigureAccount(
    const TradingAccountSnapshot& account) noexcept {
  if (storage_ != nullptr || accept_requests_ || stopping_) {
    return {Fail(ErrorCode::kInvalidState, "trading is already configured"),
            {}};
  }
  if (account.type == model::AccountType::kFutures) {
    auto storage = std::unique_ptr<Storage>(new (std::nothrow) Storage);
    if (storage == nullptr) {
      return {Fail(ErrorCode::kResourceExhausted,
                   "cannot allocate fixed trading storage"),
              {}};
    }
    const auto configured = storage->futures.Configure(account, &instruments_);
    if (configured != ErrorCode::kOk) {
      (void)Fail(configured, storage->futures.error_detail());
      return {configured, {}};
    }
    storage->type = model::AccountType::kFutures;
    storage_ = std::move(storage);
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
  for (size_t index = 0; index < account.position_count; ++index) {
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
    for (size_t previous = 0; previous < index; ++previous) {
      if (account.positions[previous].instrument_id == position.instrument_id) {
        return {Fail(ErrorCode::kInvalidArgument, "duplicate initial position"),
                {}};
      }
    }
  }
  auto storage = std::unique_ptr<Storage>(new (std::nothrow) Storage);
  if (storage == nullptr) {
    return {Fail(ErrorCode::kResourceExhausted,
                 "cannot allocate fixed trading storage"),
            {}};
  }
  storage->cash = {account.cash_balance, 0, account.cash_balance};
  storage->position_count = account.position_count;
  for (size_t index = 0; index < account.position_count; ++index) {
    storage->positions[index] = account.positions[index];
  }
  storage_ = std::move(storage);
  error_detail_[0] = '\0';
  return {};
}

void TradingEngine::Reset() noexcept {
  (void)Stop();
  storage_.reset();
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
    if (prepared_) {
      adapter_.BeginStopping();
    }
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

bool TradingEngine::broker_managed() const noexcept {
  return storage_ != nullptr && storage_->broker_managed;
}

ErrorCode TradingEngine::ValidateBrokerAccount(
    const BrokerAccountSnapshot& account) noexcept {
  if (account.revision == 0 ||
      account.position_count > account.positions.size() ||
      account.account.margin < 0 || account.account.reserved < 0 ||
      account.account.commission < 0) {
    return Fail(ErrorCode::kCorrupt, "invalid broker account fields");
  }
  for (size_t i = 0; i < account.position_count; ++i) {
    const auto& position = account.positions[i];
    model::InstrumentDefinition instrument;
    if (!instruments_.TryGetInstrument(position.instrument_id, instrument) ||
        instrument.type != model::InstrumentType::kFutures ||
        instrument.quantity_step <= 0 || position.margin < 0) {
      return Fail(ErrorCode::kCorrupt,
                  "broker holding is outside the futures dictionary");
    }
    __int128 gross = 0;
    for (const auto* bucket :
         {&position.long_today, &position.long_yesterday, &position.short_today,
          &position.short_yesterday}) {
      if (bucket->quantity < 0 || bucket->frozen_qty < 0 ||
          bucket->frozen_qty > bucket->quantity || bucket->open_notional < 0 ||
          bucket->quantity % instrument.quantity_step != 0 ||
          bucket->frozen_qty % instrument.quantity_step != 0) {
        return Fail(ErrorCode::kCorrupt, "invalid broker position bucket");
      }
      gross += bucket->quantity;
    }
    if (gross > INT64_MAX) {
      return Fail(ErrorCode::kCorrupt, "broker gross holding overflows");
    }
    const __int128 long_available =
        static_cast<__int128>(position.long_today.quantity) +
        position.long_yesterday.quantity - position.long_today.frozen_qty -
        position.long_yesterday.frozen_qty;
    const __int128 short_available =
        static_cast<__int128>(position.short_today.quantity) +
        position.short_yesterday.quantity - position.short_today.frozen_qty -
        position.short_yesterday.frozen_qty;
    if (position.unallocated_long_frozen < 0 ||
        position.unallocated_short_frozen < 0 ||
        position.unallocated_long_frozen > long_available ||
        position.unallocated_short_frozen > short_available ||
        position.unallocated_long_frozen % instrument.quantity_step != 0 ||
        position.unallocated_short_frozen % instrument.quantity_step != 0) {
      return Fail(ErrorCode::kCorrupt, "invalid unallocated broker freeze");
    }
    for (size_t j = 0; j < i; ++j) {
      if (account.positions[j].instrument_id == position.instrument_id) {
        return Fail(ErrorCode::kCorrupt, "duplicate broker holding");
      }
    }
  }
  if (broker_managed()) {
    const auto& old = storage_->broker_account;
    if (account.revision < old.revision) {
      return Fail(ErrorCode::kCorrupt, "broker account revision regressed");
    }
    if (account.revision == old.revision) {
      if (account.account != old.account ||
          account.position_count != old.position_count) {
        return Fail(ErrorCode::kCorrupt,
                    "broker account changed at the same revision");
      }
      for (size_t i = 0; i < account.position_count; ++i) {
        if (account.positions[i] != old.positions[i]) {
          return Fail(ErrorCode::kCorrupt,
                      "broker holding changed at the same revision");
        }
      }
    }
  }
  return ErrorCode::kOk;
}

uint64_t TradingEngine::mutation_revision() const noexcept {
  return storage_ != nullptr ? storage_->mutation_revision : 0;
}

uint64_t TradingEngine::next_order_id() const noexcept {
  return storage_ != nullptr ? storage_->next_order_id : 1;
}

uint64_t TradingEngine::next_cancel_id() const noexcept {
  if (storage_ == nullptr) {
    return 1;
  }
  return storage_->last_cancel_id != UINT64_MAX ? storage_->last_cancel_id + 1
                                                : UINT64_MAX;
}

bool TradingEngine::TryGetOrderAt(size_t index,
                                  model::OrderSnapshot& out) const noexcept {
  if (storage_ == nullptr || index >= storage_->order_count) {
    return false;
  }
  out = storage_->orders[index].core.snapshot();
  return true;
}

size_t TradingEngine::fill_count() const noexcept {
  return storage_ != nullptr ? storage_->fill_count : 0;
}

bool TradingEngine::TryGetFillAt(size_t index,
                                 model::Fill& out) const noexcept {
  if (storage_ == nullptr || index >= storage_->fill_count) {
    return false;
  }
  out = storage_->fills[index];
  return true;
}

base::Status TradingEngine::SetNextOrderIdAtLeast(uint64_t id) noexcept {
  if (storage_ == nullptr || accept_requests_ || id == 0) {
    return {Fail(ErrorCode::kInvalidState, "invalid cold order-ID reservation"),
            {}};
  }
  if (id > storage_->next_order_id) {
    storage_->next_order_id = id;
  }
  return {};
}

base::Status TradingEngine::SetNextCancelIdAtLeast(uint64_t id) noexcept {
  if (storage_ == nullptr || accept_requests_ || id == 0) {
    return {
        Fail(ErrorCode::kInvalidState, "invalid cold cancel-ID reservation"),
        {}};
  }
  if (id - 1 > storage_->last_cancel_id) {
    storage_->last_cancel_id = id - 1;
  }
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
  if (storage_->type != model::AccountType::kCash) {
    return ErrorCode::kOk;
  }
  if (storage_->mutation_revision == UINT64_MAX) {
    return Fail(ErrorCode::kResourceExhausted, "ledger revision exhausted");
  }
  fact.format_version = storage_->format_version;
  fact.revision = storage_->mutation_revision + 1;
  if (sink_ != nullptr && !replaying_) {
    invoking_sink_ = true;
    const auto status = sink_->BeforeCommit(fact);
    invoking_sink_ = false;
    if (!status.ok()) {
      return Fail(status.code, "ledger journal rejected mutation");
    }
  }
  return ErrorCode::kOk;
}

ErrorCode TradingEngine::Fail(ErrorCode code, const char* detail) noexcept {
  ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_), "{}", detail);
  return code;
}

std::optional<size_t> TradingEngine::FindIndex(
    model::ClientOrderId id) const noexcept {
  if (storage_ == nullptr || id.value == 0) {
    return {};
  }
  auto slot = TradingOrderHash(id);
  while (storage_->order_lookup[slot] != 0) {
    const auto index = storage_->order_lookup[slot] - 1;
    if (storage_->orders[index].core.snapshot().client_order_id == id) {
      return index;
    }
    slot = (slot + 1) & (kOrderLookupCapacity - 1);
  }
  return {};
}

std::optional<size_t> TradingEngine::FindPosition(
    model::InstrumentId id) const noexcept {
  if (storage_ == nullptr) {
    return {};
  }
  for (size_t index = 0; index < storage_->position_count; ++index) {
    if (storage_->positions[index].instrument_id == id) {
      return index;
    }
  }
  return {};
}

ErrorCode TradingEngine::ValidateSubmission(const model::OrderRequest& request,
                                            Submission& transaction) noexcept {
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
      s.next_order_id == std::numeric_limits<uint64_t>::max() ||
      s.executor.request_count() == kMaxTradingRequests) {
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
  if (futures && !SupportsPositionEffect(request.position_effect,
                                         s.broker_managed, instrument.venue)) {
    return Fail(ErrorCode::kInvalidArgument,
                "unsupported closing effect for venue");
  }
  constexpr std::array<int64_t, 7> kPriceScales = {1000000, 100000, 10000, 1000,
                                                   100,     10,     1};
  const auto scale = kPriceScales[instrument.price_decimals];
  int64_t notional = 0;
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
  transaction.futures = futures;
  transaction.scale = scale;
  transaction.notional = notional;
  transaction.position_index =
      existing_position ? *existing_position : s.position_count;
  transaction.new_position = !existing_position;
  return ErrorCode::kOk;
}

ErrorCode TradingEngine::ReserveSubmission(const model::OrderRequest& request,
                                           Submission& transaction) noexcept {
  auto& s = *storage_;
  const auto futures = transaction.futures;
  const auto notional = transaction.notional;
  auto& position = transaction.position;
  position.instrument_id = request.instrument_id;
  if (!transaction.new_position) {
    position = s.positions[transaction.position_index];
  }
  auto& cash = transaction.cash;
  cash = s.cash;
  auto& futures_reservation = transaction.futures_reservation;
  if (futures && !s.broker_managed) {
    // All fallible order/request checks precede the atomic account claim.
    // After it succeeds, the remaining order publication cannot fail.
    const auto reserved = s.futures.Reserve(request, futures_reservation);
    if (reserved != ErrorCode::kOk) {
      return Fail(reserved, s.futures.error_detail());
    }
  } else if (!futures && request.side == model::OrderSide::kBuy) {
    if (notional > cash.available) {
      return Fail(ErrorCode::kResourceExhausted, "insufficient available cash");
    }
    // notional <= available and reserved + available == balance <= INT64_MAX.
    cash.reserved += notional;
    cash.available -= notional;
  } else if (!futures) {
    if (request.quantity > position.available_qty) {
      return Fail(ErrorCode::kResourceExhausted,
                  "insufficient sellable shares");
    }
    position.available_qty -= request.quantity;
    position.reserved_qty += request.quantity;
  }
  return ErrorCode::kOk;
}

void TradingEngine::CommitSubmission(const model::OrderRequest& request,
                                     const Submission& transaction) noexcept {
  auto& s = *storage_;
  const auto index = s.order_count;
  const auto& order = transaction.order;
  const auto scale = transaction.scale;
  const auto notional = transaction.notional;
  const auto futures = transaction.futures;
  const auto& position = transaction.position;
  const auto& cash = transaction.cash;
  const auto& futures_reservation = transaction.futures_reservation;
  auto& record = s.orders[index];
  record.core = transaction.core;
  record.position_index = transaction.position_index;
  record.price_scale = scale;
  record.reserved_cash = request.side == model::OrderSide::kBuy ? notional : 0;
  record.reserved_qty = !futures && request.side == model::OrderSide::kSell
                            ? request.quantity
                            : 0;
  record.futures_reservation = futures_reservation;
  if (!futures) {
    s.positions[record.position_index] = position;
    if (transaction.new_position) {
      ++s.position_count;
    }
    s.cash = cash;
  }
  s.executor.Commit(transaction.execution);
  auto order_slot = TradingOrderHash(order.client_order_id);
  while (s.order_lookup[order_slot] != 0) {
    order_slot = (order_slot + 1) & (kOrderLookupCapacity - 1);
  }
  s.order_lookup[order_slot] = static_cast<uint16_t>(index + 1);
  ++s.order_count;
  ++s.active_count;
  ++s.next_order_id;
  if (s.type == model::AccountType::kCash) {
    ++s.mutation_revision;
  }
}

ErrorCode TradingEngine::SubmitOrder(const model::OrderRequest& request,
                                     model::ClientOrderId& out) noexcept {
  if (!accepting_orders()) {
    return Fail(ErrorCode::kInvalidState, "trading requests are not enabled");
  }
  Submission transaction;
  auto code = ValidateSubmission(request, transaction);
  if (code != ErrorCode::kOk) {
    return code;
  }
  model::InstrumentDefinition instrument;
  if (!instruments_.TryGetInstrument(request.instrument_id, instrument)) {
    return Fail(ErrorCode::kInvalidArgument, "unknown order instrument");
  }
  OrderEvent input;
  input.kind = OrderEventKind::kCreated;
  input.client_order_id = {storage_->next_order_id};
  input.now = replaying_ ? replay_now_ : clock_.Now();
  input.request = request;
  input.rules = {instrument.price_tick, instrument.quantity_step,
                 storage_->broker_managed};
  const auto applied = transaction.core.Apply(input);
  if (!applied.ok()) {
    return Fail(applied.code, applied.diagnostic.text);
  }
  transaction.order = transaction.core.snapshot();
  transaction.execution = storage_->executor.PrepareQueueNew(
      storage_->order_count, input.client_order_id);
  if (!transaction.execution.ok()) {
    return Fail(transaction.execution.code, transaction.execution.detail);
  }
  code = ReserveSubmission(request, transaction);
  if (code != ErrorCode::kOk) {
    return code;
  }
  const auto& order = transaction.order;
  TradingMutation fact;
  fact.now = replaying_ ? replay_now_ : clock_.Now();
  fact.kind = TradingMutationKind::kNewOrder;
  fact.client_order_id = order.client_order_id;
  fact.request = request;
  code = BeforeCommit(fact);
  if (code != ErrorCode::kOk) {
    return code;
  }
  CommitSubmission(request, transaction);
  out = order.client_order_id;
  return ErrorCode::kOk;
}

ErrorCode TradingEngine::QueueCancel(size_t index) noexcept {
  auto& s = *storage_;
  if (s.last_cancel_id == UINT64_MAX) {
    return Fail(ErrorCode::kResourceExhausted, "cancel identity exhausted");
  }
  auto candidate = s.orders[index].core;
  OrderEvent input;
  input.kind = OrderEventKind::kCancelRequested;
  input.client_order_id = candidate.snapshot().client_order_id;
  input.cancel_request_id = {s.last_cancel_id + 1};
  input.now = replaying_ ? replay_now_ : clock_.Now();
  const auto applied = candidate.Apply(input);
  if (!applied.ok()) {
    return Fail(applied.code, applied.diagnostic.text);
  }
  const auto execution = s.executor.PrepareQueueCancel(
      index, input.client_order_id, input.cancel_request_id);
  if (!execution.ok()) {
    return Fail(execution.code, execution.detail);
  }
  TradingMutation fact;
  fact.now = input.now;
  fact.kind = TradingMutationKind::kCancel;
  fact.client_order_id = input.client_order_id;
  fact.cancel_id = input.cancel_request_id;
  const auto journal = BeforeCommit(fact);
  if (journal != ErrorCode::kOk) {
    return journal;
  }
  s.orders[index].core = candidate;
  s.orders[index].retry_not_before_ns = 0;
  s.executor.Commit(execution);
  s.last_cancel_id = input.cancel_request_id.value;
  ++s.pending_cancel_count;
  if (s.type == model::AccountType::kCash) {
    ++s.mutation_revision;
  }
  return ErrorCode::kOk;
}

ErrorCode TradingEngine::CancelOrder(model::ClientOrderId id) noexcept {
  if (!accepting_orders()) {
    return Fail(ErrorCode::kInvalidState, "trading requests are not enabled");
  }
  const auto index = FindIndex(id);
  if (!index) {
    return Fail(ErrorCode::kInvalidArgument, "unknown client order ID");
  }
  const auto& order = storage_->orders[*index].core.snapshot();
  if (order.state != OrderState::kWorking || order.cancel_pending) {
    return Fail(ErrorCode::kInvalidState,
                "order is not cancellable or has an outstanding cancel");
  }
  return QueueCancel(*index);
}

bool TradingEngine::TryGetOrder(model::ClientOrderId id,
                                model::OrderSnapshot& out) const noexcept {
  const auto index = FindIndex(id);
  if (!index) {
    return false;
  }
  out = storage_->orders[*index].core.snapshot();
  return true;
}

bool TradingEngine::TryGetCash(model::CashSnapshot& out) const noexcept {
  if (storage_ == nullptr || storage_->type != model::AccountType::kCash) {
    return false;
  }
  out = storage_->cash;
  return true;
}

bool TradingEngine::TryGetPosition(
    model::InstrumentId id, model::PositionSnapshot& out) const noexcept {
  model::InstrumentDefinition instrument;
  if (storage_ == nullptr || storage_->type != model::AccountType::kCash ||
      !instruments_.TryGetInstrument(id, instrument) ||
      instrument.type != model::InstrumentType::kEquity) {
    return false;
  }
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
  if (broker_managed()) {
    out = storage_->broker_account.account;
    return true;
  }
  return storage_ != nullptr &&
         storage_->type == model::AccountType::kFutures &&
         storage_->futures.TryGetAccount(out);
}

bool TradingEngine::TryGetFuturesPosition(
    model::InstrumentId id,
    model::FuturesPositionSnapshot& out) const noexcept {
  if (broker_managed()) {
    model::InstrumentDefinition instrument;
    if (!instruments_.TryGetInstrument(id, instrument) ||
        instrument.type != model::InstrumentType::kFutures) {
      return false;
    }
    for (size_t i = 0; i < storage_->broker_account.position_count; ++i) {
      if (storage_->broker_account.positions[i].instrument_id == id) {
        out = storage_->broker_account.positions[i];
        return true;
      }
    }
    out = {};
    out.instrument_id = id;
    return true;
  }
  return storage_ != nullptr &&
         storage_->type == model::AccountType::kFutures &&
         storage_->futures.TryGetPosition(id, out);
}

ErrorCode TradingEngine::ApplyQuote(const model::QuoteTick& tick) noexcept {
  if (storage_ == nullptr || storage_->type == model::AccountType::kCash ||
      broker_managed()) {
    return ErrorCode::kOk;
  }
  const auto applied = storage_->futures.ApplyQuote(tick);
  return applied == ErrorCode::kOk
             ? applied
             : Fail(applied, storage_->futures.error_detail());
}

base::Status TradingEngine::ProcessRequests(uint64_t now,
                                            size_t limit) noexcept {
  error_stage_ = "trading_send";
  if (!prepared_ || storage_ == nullptr || accept_requests_) {
    return {Fail(ErrorCode::kInvalidState, "sending requires requests closed"),
            {}};
  }
  auto& s = *storage_;
  for (size_t count = 0; count < limit; ++count) {
    const auto request = s.executor.front();
    if (!request) {
      break;
    }
    const bool local = request->kind == OrderRequestKind::kNew && stopping_;
    const auto transition =
        local ? s.executor.PrepareCancelBeforeSend(request->order_index, now)
              : s.executor.PrepareBeginSend(now, s.format_version == 1);
    if (!transition.ok()) {
      if (local && transition.code == ErrorCode::kResourceExhausted) {
        break;
      }
      return {Fail(transition.code, transition.detail), {}};
    }
    TradingMutation fact;
    fact.now = now;
    fact.kind = local ? TradingMutationKind::kScheduleLocalCancel
                : request->kind == OrderRequestKind::kNew
                    ? TradingMutationKind::kSendNew
                    : TradingMutationKind::kSendCancel;
    fact.client_order_id = request->client_order_id;
    fact.cancel_id = request->cancel_request_id;
    const auto journal = BeforeCommit(fact);
    if (journal != ErrorCode::kOk) {
      return {journal, {}};
    }
    s.executor.Commit(transition);
    if (s.type == model::AccountType::kCash) {
      ++s.mutation_revision;
    }
    if (local) {
      continue;
    }
    const auto& order = s.orders[request->order_index].core.snapshot();
    base::Status result;
    switch (request->kind) {
      case OrderRequestKind::kNew: {
        const model::OrderRequest submitted{
            order.instrument_id, order.side, order.limit_price,
            order.original_qty, order.position_effect};
        result = adapter_.SendOrder(order.client_order_id, submitted, now);
        break;
      }
      case OrderRequestKind::kCancel:
        result =
            adapter_.SendCancel(order.client_order_id, order.adapter_order_id,
                                request->cancel_request_id, now);
        break;
    }
    if (!result.ok()) {
      char detail[kErrorDetailSize];
      ::yquant::base::FormatTo(detail, sizeof(detail), "{}",
                               adapter_.error_detail());
      (void)RecordRequestUncertain(*request, result.code, now);
      (void)Fail(result.code, detail);
      return result;
    }
  }
  return {};
}

ErrorCode TradingEngine::RecordRequestUncertain(
    const OrderExecutionRequest& request, ErrorCode code,
    uint64_t now) noexcept {
  auto& s = *storage_;
  const auto transition = s.executor.PrepareUncertain(request);
  if (!transition.ok()) {
    return Fail(transition.code, transition.detail);
  }
  TradingMutation fact;
  fact.kind = TradingMutationKind::kRequestUncertain;
  fact.now = now;
  fact.client_order_id = request.client_order_id;
  fact.cancel_id = request.cancel_request_id;
  fact.failure_code = code;
  const auto recorded = BeforeCommit(fact);
  if (recorded != ErrorCode::kOk) {
    return recorded;
  }
  s.executor.Commit(transition);
  if (s.type == model::AccountType::kCash) {
    ++s.mutation_revision;
  }
  return ErrorCode::kOk;
}

base::Status TradingEngine::ScheduleStopping(uint64_t now,
                                             size_t max_scan) noexcept {
  error_stage_ = "trading_drain";
  if (!stopping_ || storage_ == nullptr) {
    return {};
  }
  auto& s = *storage_;
  if (!s.scan_pending) {
    // Only due retries need a new full pass; state transitions explicitly
    // invalidate completed scans when they create newly cancellable work.
    if (!s.next_retry || *s.next_retry > now) {
      return {};
    }
    s.scan_pending = true;
    s.scan_cursor = 0;
    s.next_retry.reset();
  }
  for (size_t scanned = 0; scanned < max_scan && s.scan_cursor < s.order_count;
       ++scanned) {
    auto& record = s.orders[s.scan_cursor];
    const auto& order = record.core.snapshot();
    if (order.state == OrderState::kWorking && !order.cancel_pending &&
        record.retry_not_before_ns <= now) {
      if (s.executor.request_count() == kMaxTradingRequests) {
        return {};
      }
      const auto result = QueueCancel(s.scan_cursor);
      if (result != ErrorCode::kOk) {
        return {result, {}};
      }
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
    if (s.scan_pending) {
      s.next_retry.reset();
    }
  }
  return {};
}

std::optional<uint64_t> TradingEngine::next_deadline() const noexcept {
  if (storage_ == nullptr) {
    return {};
  }
  const auto& s = *storage_;
  if (s.executor.request_count() != 0 || s.executor.local_count() != 0 ||
      (stopping_ && s.scan_pending)) {
    return 0;
  }
  auto deadline = stopping_ ? s.next_retry : std::nullopt;
  const auto request_deadline = s.executor.next_deadline();
  if (request_deadline && (!deadline || *request_deadline < *deadline)) {
    deadline = request_deadline;
  }
  const auto adapter_deadline =
      prepared_ ? adapter_.next_deadline() : std::nullopt;
  if (adapter_deadline && (!deadline || *adapter_deadline < *deadline)) {
    deadline = adapter_deadline;
  }
  return deadline;
}

bool TradingEngine::has_ready_event(uint64_t now) const noexcept {
  const auto deadline = prepared_ ? adapter_.next_deadline() : std::nullopt;
  return has_local_updates() || IsDue(deadline, now) ||
         (storage_ != nullptr && storage_->executor.due(now).has_value());
}

bool TradingEngine::has_local_updates() const noexcept {
  return storage_ != nullptr && storage_->executor.local_count() != 0;
}
bool TradingEngine::is_drained() const noexcept {
  return storage_ == nullptr ||
         (storage_->active_count == 0 && storage_->pending_cancel_count == 0 &&
          storage_->executor.is_drained() &&
          (!prepared_ || adapter_.is_drained()));
}
size_t TradingEngine::order_count() const noexcept {
  return storage_ != nullptr ? storage_->order_count : 0;
}

ErrorCode TradingEngine::ApplyLocalUpdate(TradingDispatch& out) noexcept {
  error_stage_ = "trading_local_update";
  if (storage_ == nullptr) {
    return Fail(ErrorCode::kInvalidState, "no local order update available");
  }
  auto& s = *storage_;
  const auto local = s.executor.local_front();
  if (!local) {
    return Fail(ErrorCode::kInvalidState, "no local order update available");
  }
  const auto index = local->request.order_index;
  ReportTransaction transaction;
  transaction.core = s.orders[index].core;
  OrderEvent input;
  input.client_order_id = local->request.client_order_id;
  input.cancel_request_id = local->request.cancel_request_id;
  input.now = replaying_ ? replay_now_ : clock_.Now();
  switch (local->kind) {
    case OrderLocalResultKind::kCanceledBeforeSend:
      input.kind = OrderEventKind::kCanceledBeforeSend;
      input.reason.Set(0, "canceled before sending during shutdown");
      break;
    case OrderLocalResultKind::kCancelNotSent:
      input.kind = OrderEventKind::kCancelNotSent;
      input.reason.Set(0, "order finished before cancellation was sent");
      break;
  }
  const auto applied = transaction.core.Apply(input);
  if (!applied.ok()) {
    return Fail(applied.code, applied.diagnostic.text);
  }
  transaction.next = transaction.core.snapshot();
  transaction.dispatch = DispatchOrder(applied);
  for (size_t i = 0; i < applied.action_count; ++i) {
    transaction.release_remaining |=
        applied.actions[i] == OrderActionKind::kReleaseRemainingReservation;
  }
  transaction.execution = s.executor.PrepareConsumeLocalResult();
  if (!transaction.execution.ok()) {
    return Fail(transaction.execution.code, transaction.execution.detail);
  }
  transaction.retry = s.orders[index].retry_not_before_ns;
  const auto account = PrepareReportAccount(index, transaction);
  if (account != ErrorCode::kOk) {
    return account;
  }
  TradingMutation fact;
  fact.now = input.now;
  fact.kind = input.kind == OrderEventKind::kCanceledBeforeSend
                  ? TradingMutationKind::kLocalCancel
                  : TradingMutationKind::kCancelNotSent;
  fact.client_order_id = input.client_order_id;
  fact.cancel_id = input.cancel_request_id;
  const auto recorded = BeforeCommit(fact);
  if (recorded != ErrorCode::kOk) {
    return recorded;
  }
  CommitReport(index, transaction);
  out = transaction.dispatch;
  return ErrorCode::kOk;
}

ErrorCode TradingEngine::PrepareOrderTransition(
    const model::TradingEvent& event, size_t index, uint64_t transaction_now,
    ReportTransaction& transaction) noexcept {
  auto& s = *storage_;
  const auto& old = s.orders[index].core.snapshot();
  transaction.core = s.orders[index].core;
  OrderEvent input;
  input.kind = OrderEventKind::kBrokerReport;
  input.client_order_id = event.client_order_id;
  input.now = transaction_now;
  input.report = event;
  const auto applied = transaction.core.Apply(input);
  if (!applied.ok()) {
    return Fail(applied.code, applied.diagnostic.text);
  }
  if (applied.outcome == OrderApplyOutcome::kIgnored) {
    transaction.ignored = true;
    return ErrorCode::kOk;
  }
  transaction.next = transaction.core.snapshot();
  transaction.dispatch = DispatchOrder(applied);
  for (size_t i = 0; i < applied.action_count; ++i) {
    transaction.release_remaining |=
        applied.actions[i] == OrderActionKind::kReleaseRemainingReservation;
  }
  transaction.execution = s.executor.PrepareReport(
      index, event, transaction.next, s.format_version >= 2);
  if (!transaction.execution.ok()) {
    return Fail(transaction.execution.code, transaction.execution.detail);
  }
  transaction.bind_broker = old.adapter_order_id.empty() &&
                            !transaction.next.adapter_order_id.empty();
  if (transaction.bind_broker) {
    auto slot = TradingBrokerHash(transaction.next.adapter_order_id);
    while (s.broker_lookup[slot] != 0) {
      if (s.orders[s.broker_lookup[slot] - 1]
              .core.snapshot()
              .adapter_order_id == transaction.next.adapter_order_id) {
        return Fail(ErrorCode::kCorrupt,
                    "adapter order ID already belongs to another order");
      }
      slot = (slot + 1) & (kBrokerLookupCapacity - 1);
    }
    transaction.broker_slot = slot;
  }
  if (transaction.dispatch.has_fill) {
    if (s.fill_count == kMaxTradingFills) {
      return Fail(ErrorCode::kResourceExhausted,
                  "execution ledger capacity exhausted");
    }
    auto slot = TradingFillHash(event.client_order_id, event.fill.execution_id);
    while (s.fill_lookup[slot] != 0) {
      const auto& previous = s.fills[s.fill_lookup[slot] - 1];
      if (previous.client_order_id == event.client_order_id &&
          previous.execution_id == event.fill.execution_id) {
        return Fail(ErrorCode::kCorrupt,
                    "execution ID repeated with a new sequence");
      }
      slot = (slot + 1) & (kFillLookupCapacity - 1);
    }
    transaction.fill_slot = slot;
  }
  transaction.retry = s.orders[index].retry_not_before_ns;
  if (event.kind == OrderUpdateKind::kCancelRejected &&
      old.state == OrderState::kWorking) {
    if (transaction_now > UINT64_MAX - kCancelRetryNs) {
      return Fail(ErrorCode::kResourceExhausted,
                  "cancel retry deadline overflows");
    }
    transaction.retry = transaction_now + kCancelRetryNs;
  }
  return ErrorCode::kOk;
}

ErrorCode TradingEngine::PrepareReportAccount(
    size_t index, ReportTransaction& transaction) noexcept {
  auto& s = *storage_;
  auto& record = s.orders[index];
  const auto& old = record.core.snapshot();
  const auto& dispatch = transaction.dispatch;
  auto& cash = transaction.cash;
  cash = s.cash;
  auto& position = transaction.position;
  position = s.positions[record.position_index];
  auto& reserved_cash = transaction.reserved_cash;
  reserved_cash = record.reserved_cash;
  auto& reserved_qty = transaction.reserved_qty;
  reserved_qty = record.reserved_qty;
  if (s.type == model::AccountType::kFutures && !s.broker_managed) {
    auto reservation = record.futures_reservation;
    ErrorCode applied = ErrorCode::kOk;
    if (dispatch.has_fill) {
      applied = s.futures.ApplyFill(old, dispatch.fill, reservation);
    } else if (transaction.release_remaining) {
      applied = s.futures.Release(old, reservation);
    }
    if (applied != ErrorCode::kOk) {
      return Fail(applied, s.futures.error_detail());
    }
    record.futures_reservation = reservation;
  } else if (s.type == model::AccountType::kCash && dispatch.has_fill) {
    return PrepareCashExecution(index, transaction);
  } else if (s.type == model::AccountType::kCash &&
             transaction.release_remaining) {
    if (!ReleaseReservation(cash, position, reserved_cash, reserved_qty)) {
      return Fail(ErrorCode::kCorrupt,
                  "terminal order reservation is inconsistent");
    }
    reserved_cash = 0;
    reserved_qty = 0;
  }
  return ErrorCode::kOk;
}

ErrorCode TradingEngine::PrepareCashExecution(
    size_t index, ReportTransaction& transaction) noexcept {
  const auto& record = storage_->orders[index];
  const auto& old = record.core.snapshot();
  const auto& next = transaction.next;
  const auto& dispatch = transaction.dispatch;
  auto& cash = transaction.cash;
  auto& position = transaction.position;
  auto& reserved_cash = transaction.reserved_cash;
  auto& reserved_qty = transaction.reserved_qty;
  const auto& fill = dispatch.fill;
  int64_t actual_cash;
  if (!CashAmount(fill.price, fill.quantity, record.price_scale, actual_cash)) {
    return Fail(ErrorCode::kCorrupt, "execution cash amount overflows");
  }
  if (old.side == model::OrderSide::kBuy) {
    int64_t limit_cash;
    if (!CashAmount(old.limit_price, fill.quantity, record.price_scale,
                    limit_cash) ||
        limit_cash > reserved_cash || actual_cash > cash.balance ||
        fill.quantity >
            std::numeric_limits<int64_t>::max() - position.total_qty ||
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
    if (fill.quantity > reserved_qty || fill.quantity > position.reserved_qty ||
        fill.quantity > position.total_qty ||
        actual_cash > std::numeric_limits<int64_t>::max() - cash.balance) {
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
  return ErrorCode::kOk;
}

void TradingEngine::CommitReport(size_t index,
                                 ReportTransaction& transaction) noexcept {
  auto& s = *storage_;
  auto& record = s.orders[index];
  const auto& old = record.core.snapshot();
  const auto& next = transaction.next;
  auto& dispatch = transaction.dispatch;
  const auto fill_slot = transaction.fill_slot;
  const auto broker_slot = transaction.broker_slot;
  const auto bind_broker = transaction.bind_broker;
  const auto& cash = transaction.cash;
  const auto& position = transaction.position;
  const auto reserved_cash = transaction.reserved_cash;
  const auto reserved_qty = transaction.reserved_qty;
  const auto retry = transaction.retry;
  // All failure paths precede this transaction: no partial fill, sequence or
  // request completion can escape a rejected source event.
  if (dispatch.has_fill) {
    s.fills[s.fill_count] = dispatch.fill;
    s.fill_lookup[fill_slot] = static_cast<uint16_t>(s.fill_count + 1);
    ++s.fill_count;
  }
  if (bind_broker) {
    s.broker_lookup[broker_slot] = static_cast<uint16_t>(index + 1);
  }
  if (!model::IsTerminalOrderState(old.state) &&
      model::IsTerminalOrderState(next.state)) {
    --s.active_count;
  }
  if (old.cancel_pending && !next.cancel_pending) {
    --s.pending_cancel_count;
  }
  record.core = transaction.core;
  s.executor.Commit(transaction.execution);
  if (s.type == model::AccountType::kCash) {
    s.cash = cash;
    s.positions[record.position_index] = position;
  }
  record.reserved_cash = reserved_cash;
  record.reserved_qty = reserved_qty;
  record.retry_not_before_ns = retry;
  ScheduleReportRescan(index);
  dispatch.update.order = next;
  if (s.type == model::AccountType::kCash) {
    ++s.mutation_revision;
  }
}

void TradingEngine::ScheduleReportRescan(size_t index) noexcept {
  auto& s = *storage_;
  if (stopping_) {
    if (!s.scan_pending) {
      s.scan_pending = true;
      s.scan_cursor = 0;
      s.next_retry.reset();
    } else if (index < s.scan_cursor) {
      // Finish the current bounded pass instead of repeatedly restarting it
      // when reports arrive; revisit an already-scanned slot on the next pass.
      s.rescan_required = true;
    }
  }
}

ErrorCode TradingEngine::ApplyEvent(const model::TradingEvent& event,
                                    TradingDispatch& out) noexcept {
  error_stage_ = "trading_event";
  const auto transaction_now = replaying_ ? replay_now_ : clock_.Now();
  const auto index = FindIndex(event.client_order_id);
  if (!index) {
    return Fail(ErrorCode::kCorrupt, "report for an unknown order");
  }
  ReportTransaction transaction;
  auto result =
      PrepareOrderTransition(event, *index, transaction_now, transaction);
  if (result != ErrorCode::kOk) {
    return result;
  }
  if (transaction.ignored) {
    out = {};
    return ErrorCode::kOk;
  }
  result = PrepareReportAccount(*index, transaction);
  if (result != ErrorCode::kOk) {
    return result;
  }
  TradingMutation fact;
  fact.now = transaction_now;
  fact.kind = TradingMutationKind::kReport;
  fact.client_order_id = event.client_order_id;
  fact.event = event;
  result = BeforeCommit(fact);
  if (result != ErrorCode::kOk) {
    return result;
  }
  CommitReport(*index, transaction);
  out = transaction.dispatch;
  return ErrorCode::kOk;
}

base::Status TradingEngine::ReplayMutation(const TradingMutation& fact,
                                           TradingDispatch* out) noexcept {
  if (storage_ != nullptr && storage_->type != model::AccountType::kCash) {
    return {Fail(ErrorCode::kUnsupported,
                 "futures ledger persistence is unsupported"),
            {}};
  }
  if (storage_ == nullptr || accept_requests_ || replaying_ ||
      storage_->mutation_revision == UINT64_MAX ||
      fact.revision != storage_->mutation_revision + 1) {
    return {Fail(ErrorCode::kCorrupt, "invalid replay state or revision"), {}};
  }
  auto& s = *storage_;
  const auto index = FindIndex(fact.client_order_id);
  if (fact.kind != TradingMutationKind::kNewOrder && !index) {
    return {Fail(ErrorCode::kCorrupt, "unknown replay order"), {}};
  }
  if (!IsValidTradingMutation(fact)) {
    return {Fail(ErrorCode::kCorrupt, "unknown replay mutation"), {}};
  }
  if (fact.format_version != 1 && fact.format_version != 2) {
    return {Fail(ErrorCode::kCorrupt, "unsupported replay rules version"), {}};
  }
  const auto previous_version = s.format_version;
  if (s.mutation_revision == 0 && s.order_count == 0) {
    s.format_version = fact.format_version;
  }
  if (s.format_version != fact.format_version) {
    s.format_version = previous_version;
    return {Fail(ErrorCode::kCorrupt, "mixed replay rules versions"), {}};
  }
  replaying_ = true;
  replay_now_ = fact.now;
  ErrorCode result = ErrorCode::kOk;
  TradingDispatch dispatch;
  switch (fact.kind) {
    case TradingMutationKind::kNewOrder: {
      if (fact.client_order_id.value != s.next_order_id) {
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
          s.orders[*index].core.snapshot().state != OrderState::kWorking ||
          s.orders[*index].core.snapshot().cancel_pending) {
        result = Fail(ErrorCode::kCorrupt, "invalid replay cancel");
      } else {
        result = QueueCancel(*index);
      }
      break;
    case TradingMutationKind::kSendNew:
    case TradingMutationKind::kSendCancel:
    case TradingMutationKind::kScheduleLocalCancel: {
      const auto request = s.executor.front();
      if (!request || request->order_index != *index ||
          request->cancel_request_id != fact.cancel_id ||
          (request->kind == OrderRequestKind::kNew) !=
              (fact.kind != TradingMutationKind::kSendCancel)) {
        result = Fail(ErrorCode::kCorrupt, "replay request mismatch");
        break;
      }
      const auto next =
          fact.kind == TradingMutationKind::kScheduleLocalCancel
              ? s.executor.PrepareCancelBeforeSend(*index, fact.now)
              : s.executor.PrepareBeginSend(fact.now, s.format_version == 1);
      if (!next.ok()) {
        result = Fail(next.code, next.detail);
        break;
      }
      s.executor.Commit(next);
      ++s.mutation_revision;
      break;
    }
    case TradingMutationKind::kLocalCancel:
    case TradingMutationKind::kCancelNotSent: {
      const auto local = s.executor.local_front();
      if (!local || local->request.order_index != *index ||
          local->request.cancel_request_id != fact.cancel_id ||
          (local->kind == OrderLocalResultKind::kCanceledBeforeSend) !=
              (fact.kind == TradingMutationKind::kLocalCancel)) {
        result = Fail(ErrorCode::kCorrupt, "replay local result mismatch");
      } else {
        result = ApplyLocalUpdate(dispatch);
      }
      break;
    }
    case TradingMutationKind::kRequestUncertain: {
      const OrderExecutionRequest request{
          fact.cancel_id.value == 0 ? OrderRequestKind::kNew
                                    : OrderRequestKind::kCancel,
          *index, fact.client_order_id, fact.cancel_id};
      result = RecordRequestUncertain(request, fact.failure_code, fact.now);
      break;
    }
    case TradingMutationKind::kReport:
      if (fact.event.client_order_id != fact.client_order_id) {
        result = Fail(ErrorCode::kCorrupt, "replay report identity mismatch");
      } else {
        result = ApplyEvent(fact.event, dispatch);
      }
      break;
  }
  replaying_ = false;
  replay_now_ = 0;
  if (result != ErrorCode::kOk) {
    s.format_version = previous_version;
  }
  if (result == ErrorCode::kOk && s.mutation_revision != fact.revision) {
    return {Fail(ErrorCode::kCorrupt, "replay fact did not commit"), {}};
  }
  if (result == ErrorCode::kOk && out != nullptr) {
    *out = dispatch;
  }
  return {result, {}};
}

void TradingEngine::AbandonRecoveredRequests() noexcept {
  if (storage_ == nullptr || accept_requests_) {
    return;
  }
  auto& s = *storage_;
  s.executor.AbandonRecoveredRequests();
  s.format_version = 2;
  s.scan_cursor = 0;
  s.scan_pending = false;
  s.rescan_required = false;
  s.next_retry.reset();
}
base::Status TradingEngine::FailStage(base::Status status, const char* stage,
                                      const char* detail) noexcept {
  error_stage_ = stage;
  ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_), "{}", detail);
  return status;
}

base::Status TradingEngine::Init(const TradingConfig& config,
                                 const md::MarketDataConfig&) noexcept {
  if (prepared_ || storage_ != nullptr || accept_requests_ || stopping_) {
    return FailStage({base::ErrorCode::kInvalidState, {}}, "trading_prepare",
                     "gateway engine already initialized");
  }
  if (config.adapter != TradingAdapterKind::kMock &&
      config.adapter != TradingAdapterKind::kSimulatedFutures &&
      config.adapter != TradingAdapterKind::kCtp) {
    return FailStage({base::ErrorCode::kInvalidArgument, {}}, "trading_prepare",
                     "gateway engine requires a broker configuration");
  }
  // Even failed Prepare may own resources; Stop must run exactly once.
  prepared_ = true;
  const auto prepared = adapter_.Prepare(config);
  if (!prepared.ok()) {
    return FailStage(prepared, "trading_prepare", adapter_.error_detail());
  }
  const auto started = adapter_.Start(clock_.Now());
  if (!started.ok()) {
    return FailStage(started, "trading_start", adapter_.error_detail());
  }
  if (config.adapter == TradingAdapterKind::kCtp) {
    BrokerAccountSnapshot account;
    const auto snapshot = adapter_.ReadBrokerAccountSnapshot(account);
    if (!snapshot.ok()) {
      return FailStage(snapshot, "trading_account", adapter_.error_detail());
    }
    const auto valid = ValidateBrokerAccount(account);
    if (valid != ErrorCode::kOk) {
      return {valid, {}};
    }
    auto storage = std::unique_ptr<Storage>(new (std::nothrow) Storage);
    if (storage == nullptr) {
      return FailStage({ErrorCode::kResourceExhausted, {}}, "trading_account",
                       "cannot allocate broker order storage");
    }
    storage->type = model::AccountType::kFutures;
    storage->broker_managed = true;
    storage->broker_account = account;
    const auto timeout =
        storage->executor.Configure(config.ctp.connect_timeout_ns);
    if (timeout != ErrorCode::kOk) {
      return FailStage({timeout, {}}, "trading_account",
                       "invalid response timeout");
    }
    storage_ = std::move(storage);
    return {};
  }
  TradingAccountSnapshot account;
  const auto snapshot = adapter_.ReadAccountSnapshot(account);
  if (!snapshot.ok()) {
    return FailStage(snapshot, "trading_account", adapter_.error_detail());
  }
  error_stage_ = "trading_account";
  return ConfigureAccount(account);
}

base::Status TradingEngine::Stop() noexcept {
  accept_requests_ = false;
  if (!prepared_) {
    return {};
  }
  prepared_ = false;
  const auto stopped = adapter_.Stop();
  if (!stopped.ok()) {
    return FailStage(stopped, "trading_stop", adapter_.error_detail());
  }
  return {};
}

base::Status TradingEngine::Poll(uint64_t now, TradingDispatch& out,
                                 bool& event) noexcept {
  error_stage_ = "trading_poll";
  if (!prepared_ || storage_ == nullptr || accept_requests_) {
    return FailStage(
        {base::ErrorCode::kInvalidState, {}}, "trading_poll",
        "polling requires an initialized runtime with requests closed");
  }
  const auto expired = storage_->executor.due(now);
  if (expired) {
    const auto marked =
        RecordRequestUncertain(*expired, ErrorCode::kIoError, now);
    if (marked != ErrorCode::kOk) {
      return {marked, {}};
    }
    return FailStage({ErrorCode::kIoError, {}}, "trading_response",
                     "request response timed out; result is uncertain");
  }
  TradingDispatch dispatch;
  if (has_local_updates() &&
      (!IsDue(adapter_.next_deadline(), now) || prefer_local_)) {
    prefer_local_ = false;
    const auto applied = ApplyLocalUpdate(dispatch);
    if (applied != base::ErrorCode::kOk) {
      return {applied, {}};
    }
  } else {
    prefer_local_ = true;
    model::TradingEvent input;
    const auto polled = adapter_.Poll(now, input);
    if (!polled.status.ok() || polled.state == TradingPollState::kError) {
      for (size_t i = 0; i < storage_->order_count; ++i) {
        const auto execution = storage_->executor.state().executions[i];
        if (execution.new_request.state == OrderRequestState::kInFlight) {
          const OrderExecutionRequest request{
              OrderRequestKind::kNew, i, execution.client_order_id, {}};
          (void)RecordRequestUncertain(request, ErrorCode::kIoError, now);
        }
        if (execution.cancel_request.state == OrderRequestState::kInFlight) {
          const OrderExecutionRequest request{OrderRequestKind::kCancel, i,
                                              execution.client_order_id,
                                              execution.cancel_request_id};
          (void)RecordRequestUncertain(request, ErrorCode::kIoError, now);
        }
      }
      return FailStage(polled.status.ok()
                           ? base::Status{base::ErrorCode::kCorrupt, {}}
                           : polled.status,
                       "trading_poll", adapter_.error_detail());
    }
    if (polled.state == TradingPollState::kIdle) {
      if (IsDue(adapter_.next_deadline(), now)) {
        return FailStage(
            {base::ErrorCode::kCorrupt, {}}, "trading_poll",
            "idle trading adapter retained an already-due deadline");
      }
      event = false;
      return {};
    }
    if (polled.state != TradingPollState::kEvent &&
        !(broker_managed() && polled.state == TradingPollState::kAccount)) {
      return FailStage({base::ErrorCode::kCorrupt, {}}, "trading_poll",
                       "adapter returned an unknown polling state");
    }
    BrokerAccountSnapshot broker;
    if (broker_managed()) {
      const auto snapshot = adapter_.ReadBrokerAccountSnapshot(broker);
      if (!snapshot.ok()) {
        return FailStage(snapshot, "trading_account", adapter_.error_detail());
      }
      const auto valid = ValidateBrokerAccount(broker);
      if (valid != ErrorCode::kOk) {
        return {valid, {}};
      }
    }
    const auto applied = polled.state == TradingPollState::kEvent
                             ? ApplyEvent(input, dispatch)
                             : ErrorCode::kOk;
    if (applied != base::ErrorCode::kOk) {
      char broker[kTradingIdHexSize];
      char execution[kTradingIdHexSize];
      CopyIdHex(input.adapter_order_id.view(), broker);
      CopyIdHex(input.fill.execution_id.view(), execution);
      error_stage_ = "trading_event";
      char detail[kErrorDetailSize];
      ::yquant::base::FormatTo(
          detail, sizeof(detail),
          "client_order_id={} sequence={} kind={} cumulative={} fill_price={} "
          "fill_qty={} cancel_request={} broker_id_hex={} execution_id_hex={}: "
          "{}",
          input.client_order_id.value, input.report_sequence,
          static_cast<unsigned>(input.kind), input.cumulative_filled_qty,
          input.fill.price, input.fill.quantity, input.cancel_request_id.value,
          broker, execution, error_detail_);
      (void)FailStage({applied, {}}, "trading_event", detail);
      // Report even when the runner retains an earlier callback failure.
      ::yquant::base::Print(stderr,
                            "yquant-trading: unapplied_trading_event {}\n",
                            error_detail_);
      return {applied, {}};
    }
    if (broker_managed()) {
      storage_->broker_account = broker;
    }
  }
  out = dispatch;
  event = true;
  return {};
}

}  // namespace yquant::trading
