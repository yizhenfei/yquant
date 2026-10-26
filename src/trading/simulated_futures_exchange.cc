#include "yquant/trading/simulated_futures_exchange.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>

#include "yquant/base/format.h"
#include "yquant/trading/trading_limits.h"

namespace yquant::trading {
namespace {

constexpr size_t kNoIndex = std::numeric_limits<size_t>::max();
constexpr size_t kPlanCapacity = kMaxTradingFills + 2 * kMaxTradingOrders;
constexpr size_t kLookupCapacity = 2 * kMaxTradingOrders;

// Each identifier owns its bytes and is unique within this exchange session.
template <typename Id>
bool MakeId(const char* prefix, uint64_t order, uint64_t ordinal,
            Id& out) noexcept {
  char bytes[65];
  const int size = ::yquant::base::FormatTo(bytes, sizeof(bytes), "{}{}-{}",
                                            prefix, order, ordinal);
  return size > 0 && static_cast<size_t>(size) < sizeof(bytes) &&
         out.Assign({bytes, static_cast<size_t>(size)}) == base::ErrorCode::kOk;
}

// Enforces presence-bit normalization before a quote can replenish liquidity.
bool ValidSide(const model::QuoteTick& quote, model::QuoteFlag flag,
               int64_t price, int64_t quantity) noexcept {
  return model::HasQuoteFlag(quote.flags, flag) ? price > 0 && quantity > 0
                                                : price == 0 && quantity == 0;
}

}  // namespace

// Retains order identities, matched-but-unpublished quantities and normalized
// plans separately. Committing a match consumes liquidity immediately; Poll
// advances published cumulative quantity only when returning that execution.
struct SimulatedFuturesExchange::Storage {
  enum class Kind { kAccepted, kFill, kCancel };
  struct Order {
    model::ClientOrderId id;
    model::AdapterOrderId broker;
    model::OrderRequest request;
    int64_t matched = 0;
    int64_t published = 0;
    uint64_t sequence = 0;
    uint64_t fill_ordinal = 0;
    uint64_t ack_due = 0;
    uint64_t cancel_due = 0;
    model::CancelRequestId cancel_id;
    model::CancelRequestId last_cancel_id;
    bool acknowledged = false;
    bool canceled = false;
  };
  struct Book {
    model::QuoteTick quote;
    int64_t bid_remaining = 0;
    int64_t ask_remaining = 0;
  };
  struct Plan {
    Kind kind = Kind::kAccepted;
    size_t order = 0;
    uint64_t due = 0;
    uint64_t registration = 0;
    int64_t quantity = 0;
    int64_t price = 0;
    model::CancelRequestId cancel_id;
  };

  TradingConfig config;
  std::array<Order, kMaxTradingOrders> orders;
  std::array<size_t, kLookupCapacity> lookup = {};
  size_t order_count = 0;
  std::array<Book, md::kMaxMarketDataInstruments> books;
  size_t book_count = 0;
  std::array<Plan, kPlanCapacity> plans;
  size_t plan_count = 0;
  size_t fill_count = 0;
  uint64_t registration = 0;

  // Returns a matching/empty hash slot, or kNoIndex for a full table.
  // Read-only.
  size_t Bucket(model::ClientOrderId id) const noexcept {
    auto hash = id.value;
    hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ULL;
    hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebULL;
    size_t slot = hash & (kLookupCapacity - 1);
    for (size_t index = 0; index < kLookupCapacity; ++index) {
      if (lookup[slot] == 0 || orders[lookup[slot] - 1].id == id) {
        return slot;
      }
      slot = (slot + 1) & (kLookupCapacity - 1);
    }
    return kNoIndex;
  }
  // Finds the owned instrument book; missing instruments do not create a slot.
  size_t BookIndex(model::InstrumentId id) const noexcept {
    for (size_t index = 0; index < book_count; ++index) {
      if (books[index].quote.instrument_id == id) {
        return index;
      }
    }
    return kNoIndex;
  }
  // Stable registration ordering resolves equal report deadlines
  // deterministically.
  static bool Earlier(const Plan& left, const Plan& right) noexcept {
    return left.due < right.due ||
           (left.due == right.due && left.registration < right.registration);
  }
  // Capacity and ordinal overflow are preflighted before these heap mutations.
  void Push(Plan plan) noexcept {
    plan.registration = ++registration;
    size_t position = plan_count++;
    while (position != 0) {
      const auto parent = (position - 1) / 2;
      if (!Earlier(plan, plans[parent])) {
        break;
      }
      plans[position] = plans[parent];
      position = parent;
    }
    plans[position] = plan;
  }
  // Removes the earliest plan. Requires nonempty heap; order facts are changed
  // by Poll only after constructing a complete normalized output event.
  Plan Pop() noexcept {
    const Plan result = plans[0];
    const Plan tail = plans[--plan_count];
    if (plan_count == 0) {
      return result;
    }
    size_t position = 0;
    for (;;) {
      const auto left = 2 * position + 1;
      if (left >= plan_count) {
        break;
      }
      auto child = left;
      if (left + 1 < plan_count && Earlier(plans[left + 1], plans[left])) {
        ++child;
      }
      if (!Earlier(plans[child], tail)) {
        break;
      }
      plans[position] = plans[child];
      position = child;
    }
    plans[position] = tail;
    return result;
  }
  // Nonmutating preflight for one entire send/quote operation. Includes
  // retained fill-history and ordinal limits, not just currently pending heap
  // storage.
  bool CanQueue(size_t plans_needed, size_t fills_needed) const noexcept {
    return plans_needed <= kPlanCapacity - plan_count &&
           fills_needed <= kMaxTradingFills - fill_count &&
           plans_needed <= UINT64_MAX - registration;
  }
  // Uses the unconsumed side of this single quote; old quotes cannot replenish
  // it. Cancel deadlines are effective even if their result has not been read.
  int64_t MatchQuantity(const Order& order, const Book& book,
                        uint64_t now) const noexcept {
    if (order.canceled || order.matched == order.request.quantity ||
        (order.cancel_id.value != 0 && now >= order.cancel_due) ||
        !model::HasQuoteFlag(book.quote.flags, model::QuoteFlag::kTradable)) {
      return 0;
    }
    if (order.request.side == model::OrderSide::kBuy) {
      if (!model::HasQuoteFlag(book.quote.flags, model::QuoteFlag::kAskValid) ||
          order.request.limit_price < book.quote.ask_price) {
        return 0;
      }
      return std::min(order.request.quantity - order.matched,
                      book.ask_remaining);
    }
    if (!model::HasQuoteFlag(book.quote.flags, model::QuoteFlag::kBidValid) ||
        order.request.limit_price > book.quote.bid_price) {
      return 0;
    }
    return std::min(order.request.quantity - order.matched, book.bid_remaining);
  }
  // Requires positive MatchQuantity and successful capacity preflight. Consumes
  // quote liquidity/matched quantity now; published cumulative stays unchanged
  // until Poll returns this fill. The plan stores values, never input
  // references.
  void CommitFill(size_t index, Book& book, int64_t quantity, int64_t price,
                  uint64_t now) noexcept {
    auto& order = orders[index];
    order.matched += quantity;
    if (order.request.side == model::OrderSide::kBuy) {
      book.ask_remaining -= quantity;
    } else {
      book.bid_remaining -= quantity;
    }
    ++fill_count;
    Push({Kind::kFill,
          index,
          std::max(now, order.ack_due),
          0,
          quantity,
          price,
          {}});
  }
};

SimulatedFuturesExchange::SimulatedFuturesExchange() noexcept = default;
SimulatedFuturesExchange::~SimulatedFuturesExchange() noexcept = default;

base::Status SimulatedFuturesExchange::Error(base::ErrorCode code,
                                             const char* detail) noexcept {
  ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_), "{}", detail);
  return {code, {}};
}
TradingPollResult SimulatedFuturesExchange::PollError(
    base::ErrorCode code, const char* detail) noexcept {
  state_ = State::kFailed;
  return {TradingPollState::kError, Error(code, detail)};
}

base::Status SimulatedFuturesExchange::Prepare(
    const TradingConfig& config) noexcept {
  if (state_ != State::kUnprepared) {
    return Error(base::ErrorCode::kInvalidState,
                 "exchange can only be prepared once");
  }
  base::ConfigError error;
  const auto validated = ValidateTradingConfig(config, error);
  if (!validated.ok()) {
    return Error(validated.code, error.message);
  }
  if (config.initial_account.type != model::AccountType::kFutures) {
    return Error(base::ErrorCode::kInvalidArgument,
                 "simulated futures exchange requires a futures account");
  }
  auto storage = std::unique_ptr<Storage>(new (std::nothrow) Storage);
  if (!storage) {
    return Error(base::ErrorCode::kResourceExhausted,
                 "cannot allocate exchange storage");
  }
  storage->config = config;
  storage_ = std::move(storage);
  state_ = State::kPrepared;
  return {};
}
base::Status SimulatedFuturesExchange::Start(uint64_t) noexcept {
  if (state_ != State::kPrepared) {
    return Error(base::ErrorCode::kInvalidState, "exchange is not prepared");
  }
  state_ = State::kActive;
  return {};
}
base::Status SimulatedFuturesExchange::ReadAccountSnapshot(
    TradingAccountSnapshot& out) noexcept {
  if (state_ != State::kActive || storage_->order_count != 0) {
    return Error(base::ErrorCode::kInvalidState,
                 "bootstrap is available only before the first order");
  }
  out = storage_->config.initial_account;
  return {};
}

base::Status SimulatedFuturesExchange::SendOrder(
    model::ClientOrderId id, const model::OrderRequest& request,
    uint64_t now) noexcept {
  if (state_ != State::kActive) {
    return Error(base::ErrorCode::kInvalidState, "exchange is not active");
  }
  auto& data = *storage_;
  const auto bucket = data.Bucket(id);
  if (id.value == 0 || bucket == kNoIndex || data.lookup[bucket] != 0 ||
      request.instrument_id.value == 0 || request.limit_price <= 0 ||
      request.quantity <= 0 ||
      (request.side != model::OrderSide::kBuy &&
       request.side != model::OrderSide::kSell) ||
      (request.position_effect != model::PositionEffect::kOpen &&
       request.position_effect != model::PositionEffect::kCloseToday &&
       request.position_effect != model::PositionEffect::kCloseYesterday)) {
    return Error(base::ErrorCode::kInvalidArgument,
                 "invalid or repeated futures order request");
  }
  if (data.order_count == data.orders.size()) {
    return Error(base::ErrorCode::kResourceExhausted,
                 "exchange order capacity exhausted");
  }
  if (now > UINT64_MAX - data.config.report_interval_ns) {
    return Error(base::ErrorCode::kResourceExhausted,
                 "acknowledgement time overflow");
  }
  Storage::Order candidate;
  candidate.id = id;
  candidate.request = request;
  candidate.ack_due = now + data.config.report_interval_ns;
  if (!MakeId("FX-", id.value, 0, candidate.broker)) {
    return Error(base::ErrorCode::kInternalError,
                 "cannot format broker identity");
  }
  const auto book_index = data.BookIndex(request.instrument_id);
  const auto quantity =
      book_index == kNoIndex
          ? 0
          : data.MatchQuantity(candidate, data.books[book_index], now);
  if (!data.CanQueue(quantity > 0 ? 2 : 1, quantity > 0 ? 1 : 0)) {
    return Error(base::ErrorCode::kResourceExhausted,
                 "exchange event capacity exhausted");
  }
  const auto index = data.order_count++;
  data.orders[index] = candidate;
  data.lookup[bucket] = index + 1;
  data.Push({Storage::Kind::kAccepted, index, candidate.ack_due, 0, 0, 0, {}});
  if (quantity > 0) {
    auto& book = data.books[book_index];
    const auto price = request.side == model::OrderSide::kBuy
                           ? book.quote.ask_price
                           : book.quote.bid_price;
    data.CommitFill(index, book, quantity, price, now);
  }
  return {};
}

base::Status SimulatedFuturesExchange::SendCancel(
    model::ClientOrderId id, const model::AdapterOrderId& broker,
    model::CancelRequestId cancel, uint64_t now) noexcept {
  if (state_ != State::kActive) {
    return Error(base::ErrorCode::kInvalidState, "exchange is not active");
  }
  auto& data = *storage_;
  const auto bucket = data.Bucket(id);
  if (id.value == 0 || cancel.value == 0 || bucket == kNoIndex ||
      data.lookup[bucket] == 0) {
    return Error(base::ErrorCode::kInvalidArgument,
                 "unknown order or empty cancel identity");
  }
  auto& order = data.orders[data.lookup[bucket] - 1];
  if (order.broker != broker || !order.acknowledged ||
      order.cancel_id.value != 0 ||
      cancel.value <= order.last_cancel_id.value || order.canceled) {
    return Error(base::ErrorCode::kInvalidState,
                 "invalid cancel state or broker identity");
  }
  if (now > UINT64_MAX - data.config.report_interval_ns ||
      !data.CanQueue(1, 0)) {
    return Error(base::ErrorCode::kResourceExhausted,
                 "cancel time or event capacity exhausted");
  }
  order.cancel_id = cancel;
  order.last_cancel_id = cancel;
  order.cancel_due = now + data.config.report_interval_ns;
  data.Push({Storage::Kind::kCancel, data.lookup[bucket] - 1, order.cancel_due,
             0, 0, 0, cancel});
  return {};
}

base::Status SimulatedFuturesExchange::OnQuote(const model::QuoteTick& quote,
                                               uint64_t now) noexcept {
  if (state_ != State::kActive) {
    return Error(base::ErrorCode::kInvalidState, "exchange is not active");
  }
  if (quote.instrument_id.value == 0 || quote.source_id.value == 0 ||
      quote.sequence == 0 || quote.receive_time_ns > now ||
      (quote.flags & ~model::kKnownQuoteFlags) != 0 ||
      !ValidSide(quote, model::QuoteFlag::kBidValid, quote.bid_price,
                 quote.bid_qty) ||
      !ValidSide(quote, model::QuoteFlag::kAskValid, quote.ask_price,
                 quote.ask_qty)) {
    return Error(base::ErrorCode::kInvalidArgument, "invalid normalized quote");
  }
  auto& data = *storage_;
  auto index = data.BookIndex(quote.instrument_id);
  if (index != kNoIndex &&
      quote.source_id == data.books[index].quote.source_id &&
      quote.sequence <= data.books[index].quote.sequence) {
    return {};
  }
  if (index == kNoIndex && data.book_count == data.books.size()) {
    return Error(base::ErrorCode::kResourceExhausted,
                 "exchange book capacity exhausted");
  }
  Storage::Book candidate{quote, quote.bid_qty, quote.ask_qty};
  auto remaining = candidate;
  size_t fills = 0;
  for (size_t current = 0; current < data.order_count; ++current) {
    const auto& order = data.orders[current];
    if (order.request.instrument_id != quote.instrument_id) {
      continue;
    }
    const auto quantity = data.MatchQuantity(order, remaining, now);
    if (quantity == 0) {
      continue;
    }
    ++fills;
    if (order.request.side == model::OrderSide::kBuy) {
      remaining.ask_remaining -= quantity;
    } else {
      remaining.bid_remaining -= quantity;
    }
  }
  if (!data.CanQueue(fills, fills)) {
    return Error(base::ErrorCode::kResourceExhausted,
                 "quote would exceed exchange fill/event capacity");
  }
  if (index == kNoIndex) {
    index = data.book_count++;
  }
  data.books[index] = candidate;
  for (size_t current = 0; current < data.order_count; ++current) {
    const auto& order = data.orders[current];
    if (order.request.instrument_id != quote.instrument_id) {
      continue;
    }
    const auto quantity = data.MatchQuantity(order, data.books[index], now);
    if (quantity > 0) {
      data.CommitFill(current, data.books[index], quantity,
                      order.request.limit_price, now);
    }
  }
  return {};
}

TradingPollResult SimulatedFuturesExchange::Poll(
    uint64_t now, model::TradingEvent& out) noexcept {
  if (state_ != State::kActive) {
    return {TradingPollState::kError,
            Error(base::ErrorCode::kInvalidState, "exchange is not active")};
  }
  auto& data = *storage_;
  if (data.plan_count == 0 || data.plans[0].due > now) {
    return {};
  }
  const auto& next = data.plans[0];
  const auto& pending = data.orders[next.order];
  if (pending.sequence == UINT64_MAX || (next.kind == Storage::Kind::kFill &&
                                         pending.fill_ordinal == UINT64_MAX)) {
    return PollError(base::ErrorCode::kResourceExhausted,
                     "exchange report identity exhausted");
  }
  model::TradingEvent event;
  event.client_order_id = pending.id;
  event.adapter_order_id = pending.broker;
  event.report_sequence = pending.sequence + 1;
  event.receive_time_ns = now;
  event.cumulative_filled_qty = pending.published;
  if (next.kind == Storage::Kind::kFill) {
    if (!MakeId("FE-", pending.id.value, pending.fill_ordinal + 1,
                event.fill.execution_id)) {
      return PollError(base::ErrorCode::kInternalError,
                       "cannot format execution identity");
    }
    event.kind = model::OrderUpdateKind::kFill;
    event.fill.client_order_id = pending.id;
    event.fill.adapter_order_id = pending.broker;
    event.fill.instrument_id = pending.request.instrument_id;
    event.fill.side = pending.request.side;
    event.fill.position_effect = pending.request.position_effect;
    event.fill.price = next.price;
    event.fill.quantity = next.quantity;
    event.fill.receive_time_ns = now;
    event.cumulative_filled_qty += next.quantity;
  } else if (next.kind == Storage::Kind::kCancel) {
    event.kind = pending.matched == pending.request.quantity
                     ? model::OrderUpdateKind::kCancelRejected
                     : model::OrderUpdateKind::kCanceled;
    event.cancel_request_id = next.cancel_id;
    if (pending.published != pending.matched) {
      return PollError(base::ErrorCode::kCorrupt,
                       "cancel preceded a matched execution report");
    }
    if (event.kind == model::OrderUpdateKind::kCancelRejected) {
      event.reason.Set(1, "order already fully filled");
    }
  } else {
    event.kind = model::OrderUpdateKind::kAccepted;
  }
  const auto plan = data.Pop();
  auto& order = data.orders[plan.order];
  ++order.sequence;
  if (plan.kind == Storage::Kind::kAccepted) {
    order.acknowledged = true;
  } else if (plan.kind == Storage::Kind::kFill) {
    order.published += plan.quantity;
    ++order.fill_ordinal;
  } else {
    order.cancel_id = {};
    if (event.kind == model::OrderUpdateKind::kCanceled) {
      order.canceled = true;
    }
  }
  out = event;
  return {TradingPollState::kEvent, {}};
}

std::optional<uint64_t> SimulatedFuturesExchange::next_deadline()
    const noexcept {
  return state_ == State::kActive && storage_->plan_count != 0
             ? std::optional<uint64_t>{storage_->plans[0].due}
             : std::nullopt;
}
bool SimulatedFuturesExchange::is_drained() const noexcept {
  if (state_ == State::kStopped) {
    return drained_on_stop_;
  }
  if (!storage_) {
    return true;
  }
  if (storage_->plan_count != 0) {
    return false;
  }
  for (size_t index = 0; index < storage_->order_count; ++index) {
    const auto& order = storage_->orders[index];
    if (!order.canceled && order.matched != order.request.quantity) {
      return false;
    }
  }
  return true;
}
base::Status SimulatedFuturesExchange::Stop() noexcept {
  if (state_ == State::kStopped) {
    return {};
  }
  drained_on_stop_ = is_drained();
  storage_.reset();
  state_ = State::kStopped;
  return {};
}

void SimulatedFuturesExchange::Reset() noexcept {
  (void)Stop();
  state_ = State::kUnprepared;
  drained_on_stop_ = true;
  error_detail_[0] = '\0';
}

}  // namespace yquant::trading
