#include <unistd.h>

#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <string_view>

#include "simdjson.h"
#include "yquant/model/account.h"
#include "yquant/model/instrument.h"
#include "yquant/model/order.h"
#include "yquant/model/quote_tick.h"
#include "yquant/model/trading_event.h"
#include "yquant/strategy/strategy.h"
#include "yquant/strategy/strategy_plugin.h"
#include "yquant/strategy/timer.h"

namespace {

using yquant::base::ErrorCode;
using yquant::model::CashSnapshot;
using yquant::model::Fill;
using yquant::model::InstrumentDefinition;
using yquant::model::InstrumentId;
using yquant::model::IsTerminalOrderState;
using yquant::model::OrderRequest;
using yquant::model::OrderSide;
using yquant::model::OrderSnapshot;
using yquant::model::OrderState;
using yquant::model::OrderUpdate;
using yquant::model::OrderUpdateKind;
using yquant::model::PositionSnapshot;
using yquant::model::QuoteFlag;
using yquant::model::QuoteTick;
using yquant::model::StrategyOrderId;
using yquant::strategy::ConfigError;
using yquant::strategy::Strategy;
using yquant::strategy::StrategyConfig;
using yquant::strategy::StrategyContext;
using yquant::strategy::StrategyPlugin;
using yquant::strategy::TimerEvent;

constexpr std::uint64_t kNsPerMs = 1000000;
constexpr std::int64_t kMaxQuantity = std::numeric_limits<std::int64_t>::max();
constexpr std::uint64_t kMaxCounter = std::numeric_limits<std::uint64_t>::max();

// Copies parser diagnostics before the parser and its input are destroyed.
ErrorCode ParseError(simdjson::error_code result, std::string_view path,
                     ConfigError& error) noexcept {
  error.Set(path, simdjson::error_message(result));
  return result == simdjson::MEMALLOC ? ErrorCode::kResourceExhausted
                                      : ErrorCode::kInvalidArgument;
}

// Writes a one-time hot-path marker without allocating a stdio buffer. A
// failure can leave a prefix written; interrupted writes are retried.
ErrorCode WriteMarker(std::string_view text) noexcept {
  while (!text.empty()) {
    const auto written = write(STDOUT_FILENO, text.data(), text.size());
    if (written > 0) {
      text.remove_prefix(static_cast<std::size_t>(written));
    } else if (written == -1 && errno == EINTR) {
      continue;
    } else {
      return ErrorCode::kIoError;
    }
  }
  return ErrorCode::kOk;
}

// Owns all strategy parameters. Initialization validates and commits them
// together, retaining no views into JSON or parser storage.
class BboMarketMakingConfig final : public StrategyConfig {
 public:
  std::string_view venue() const noexcept { return {venue_, venue_size_}; }
  std::string_view symbol() const noexcept {
    return {symbol_, sizeof(symbol_)};
  }
  std::int64_t quote_qty() const noexcept { return quote_qty_; }
  std::int64_t min_position() const noexcept { return min_position_; }
  std::int64_t max_position() const noexcept { return max_position_; }
  std::uint64_t quote_timeout_ns() const noexcept { return quote_timeout_ns_; }
  std::uint64_t retry_delay_ns() const noexcept { return retry_delay_ns_; }

 protected:
  ErrorCode LoadParameters(std::string_view json,
                           ConfigError& error) noexcept override {
    simdjson::padded_string input(json);
    if (input.data() == nullptr) {
      error.Set("", "could not allocate parameter input");
      return ErrorCode::kResourceExhausted;
    }
    simdjson::dom::parser parser;
    if (const auto result = parser.allocate(input.size());
        result != simdjson::SUCCESS) {
      return ParseError(result, "", error);
    }
    simdjson::dom::object object;
    if (const auto result = parser.parse(input).get_object().get(object);
        result != simdjson::SUCCESS) {
      return ParseError(result, "", error);
    }

    constexpr std::array<std::string_view, 7> kFields = {
        "venue",        "symbol",           "quote_qty",     "min_position",
        "max_position", "quote_timeout_ms", "retry_delay_ms"};
    std::array<bool, kFields.size()> seen{};
    std::string_view venue;
    std::string_view symbol;
    std::int64_t quote_qty = 0;
    std::int64_t min_position = 0;
    std::int64_t max_position = 0;
    std::uint64_t timeout_ms = 0;
    std::uint64_t retry_ms = 0;
    for (const auto field : object) {
      std::size_t index = 0;
      while (index != kFields.size() && field.key != kFields[index]) ++index;
      if (index == kFields.size() || seen[index]) {
        error.Set(field.key, index == kFields.size() ? "unknown parameter"
                                                     : "duplicate parameter");
        return ErrorCode::kInvalidArgument;
      }
      seen[index] = true;
      simdjson::error_code result = simdjson::SUCCESS;
      switch (index) {
        case 0:
          result = field.value.get_string().get(venue);
          break;
        case 1:
          result = field.value.get_string().get(symbol);
          break;
        case 2:
          result = field.value.get_int64().get(quote_qty);
          break;
        case 3:
          result = field.value.get_int64().get(min_position);
          break;
        case 4:
          result = field.value.get_int64().get(max_position);
          break;
        case 5:
          result = field.value.get_uint64().get(timeout_ms);
          break;
        case 6:
          result = field.value.get_uint64().get(retry_ms);
          break;
      }
      if (result != simdjson::SUCCESS) {
        return ParseError(result, field.key, error);
      }
    }
    for (std::size_t index = 0; index != kFields.size(); ++index) {
      if (!seen[index]) {
        error.Set(kFields[index], "required parameter is missing");
        return ErrorCode::kInvalidArgument;
      }
    }
    if (venue != "SSE" && venue != "SZSE" && venue != "BSE") {
      error.Set("venue", "must be SSE, SZSE, or BSE");
      return ErrorCode::kInvalidArgument;
    }
    InstrumentId id;
    if (yquant::model::MakeInstrumentId(venue, symbol, id) != ErrorCode::kOk) {
      error.Set("symbol", "must contain exactly six ASCII digits");
      return ErrorCode::kInvalidArgument;
    }
    if (quote_qty <= 0) {
      error.Set("quote_qty", "must be a positive integer number of shares");
      return ErrorCode::kInvalidArgument;
    }
    if (min_position < 0) {
      error.Set("min_position", "must be nonnegative");
      return ErrorCode::kInvalidArgument;
    }
    if (max_position < min_position ||
        max_position - min_position < quote_qty) {
      error.Set("max_position", "must allow at least quote_qty above minimum");
      return ErrorCode::kInvalidArgument;
    }
    if (timeout_ms == 0 || timeout_ms > 60000 || retry_ms == 0 ||
        retry_ms > 60000) {
      error.Set(timeout_ms == 0 || timeout_ms > 60000 ? "quote_timeout_ms"
                                                      : "retry_delay_ms",
                "must be an integer from 1 to 60000 milliseconds");
      return ErrorCode::kInvalidArgument;
    }

    std::memcpy(venue_, venue.data(), venue.size());
    venue_size_ = venue.size();
    std::memcpy(symbol_, symbol.data(), symbol.size());
    quote_qty_ = quote_qty;
    min_position_ = min_position;
    max_position_ = max_position;
    quote_timeout_ns_ = timeout_ms * kNsPerMs;
    retry_delay_ns_ = retry_ms * kNsPerMs;
    return ErrorCode::kOk;
  }

 private:
  char venue_[8] = {};
  std::size_t venue_size_ = 0;
  char symbol_[6] = {};
  std::int64_t quote_qty_ = 0;
  std::int64_t min_position_ = 0;
  std::int64_t max_position_ = 0;
  std::uint64_t quote_timeout_ns_ = 0;
  std::uint64_t retry_delay_ns_ = 0;
};

// Maintains at most one nonterminal order per side against the current BBO.
// The host owns execution/account facts and stop-time cancellation; this
// strategy stores only IDs, retry deadlines and counters on its callback
// thread.
class BboMarketMakingStrategy final : public Strategy {
 public:
  BboMarketMakingConfig& config() noexcept override { return config_; }

  ErrorCode OnStart(StrategyContext& context) noexcept override {
    if (!config_.ready() || started_ || context.trading().accepting_orders()) {
      return ErrorCode::kInvalidState;
    }
    const auto found = context.market_data().FindInstrument(
        config_.venue(), config_.symbol(), instrument_);
    if (found != ErrorCode::kOk) return found;
    if (instrument_.price_decimals > 6) return ErrorCode::kCorrupt;
    for (std::uint8_t decimals = instrument_.price_decimals; decimals < 6;
         ++decimals) {
      price_scale_ *= 10;
    }
    CashSnapshot cash;
    PositionSnapshot position;
    const auto account = ReadAccount(context, cash, position);
    if (account != ErrorCode::kOk) return account;
    if (cash.reserved != 0 || position.reserved_qty != 0) {
      return ErrorCode::kCorrupt;
    }
    if (position.total_qty < config_.min_position() ||
        position.total_qty > config_.max_position()) {
      return ErrorCode::kInvalidArgument;
    }
    const auto subscribed =
        context.market_data().SubscribeQuotes(instrument_.id);
    if (subscribed != ErrorCode::kOk) return subscribed;
    const int written =
        std::fprintf(stdout,
                     "bbo_market_making: ready cash_balance=%" PRId64
                     " total_qty=%" PRId64 " available_qty=%" PRId64 "\n",
                     cash.balance, position.total_qty, position.available_qty);
    if (written < 0 || std::fflush(stdout) != 0) return ErrorCode::kIoError;
    started_ = true;
    return ErrorCode::kOk;
  }

  ErrorCode OnTick(StrategyContext& context,
                   const QuoteTick& tick) noexcept override {
    if (tick.instrument_id != instrument_.id) {
      return ErrorCode::kCorrupt;
    }
    return Reconcile(context);
  }

  ErrorCode OnOrderUpdate(StrategyContext& context,
                          const OrderUpdate& update) noexcept override {
    const auto& order = update.order;
    OrderSnapshot queried;
    if (order.instrument_id != instrument_.id ||
        (order.side != OrderSide::kBuy && order.side != OrderSide::kSell) ||
        !context.trading().TryGetOrder(order.strategy_order_id, queried) ||
        queried != order) {
      return ErrorCode::kCorrupt;
    }
    auto& side = sides_[SideIndex(order.side)];
    const bool matched = side.id == order.strategy_order_id;
    if (matched && IsTerminalOrderState(order.state)) {
      side.id = {};
    }
    // Filled orders can receive the result of an earlier cancel after their
    // slot has been replaced. Such reports never alter the replacement slot
    // or its retry deadline.
    if (!context.trading().accepting_orders()) return ErrorCode::kOk;
    if (matched && (update.kind == OrderUpdateKind::kRejected ||
                    update.kind == OrderUpdateKind::kCancelRejected)) {
      const auto retried = DelayRetry(side, context.Now());
      if (retried != ErrorCode::kOk) return retried;
    }
    return Reconcile(context);
  }

  ErrorCode OnFill(StrategyContext& context,
                   const Fill& fill) noexcept override {
    OrderSnapshot order;
    if (fill.instrument_id != instrument_.id || fill.quantity <= 0 ||
        (fill.side != OrderSide::kBuy && fill.side != OrderSide::kSell) ||
        !context.trading().TryGetOrder(fill.strategy_order_id, order) ||
        order.instrument_id != fill.instrument_id || order.side != fill.side ||
        order.adapter_order_id != fill.adapter_order_id) {
      return ErrorCode::kCorrupt;
    }
    const auto quantity = static_cast<std::uint64_t>(fill.quantity);
    if (fills_ == kMaxCounter || quantity > kMaxCounter - filled_qty_) {
      return ErrorCode::kResourceExhausted;
    }
    ++fills_;
    filled_qty_ += quantity;
    // OnOrderUpdate already reconciled against the committed account. A fill
    // callback records statistics without issuing a second set of requests.
    return ErrorCode::kOk;
  }

  ErrorCode OnTimer(StrategyContext& context,
                    const TimerEvent&) noexcept override {
    return Reconcile(context);
  }

  ErrorCode OnStop(StrategyContext& context) noexcept override {
    if (!started_ || context.trading().accepting_orders()) {
      return ErrorCode::kInvalidState;
    }
    CashSnapshot cash;
    PositionSnapshot position;
    const auto account = ReadAccount(context, cash, position);
    if (account != ErrorCode::kOk) return account;
    unsigned active_slots = 0;
    for (std::size_t index = 0; index != sides_.size(); ++index) {
      OrderSnapshot order;
      const auto read = ReadSlot(context, index, order);
      if (read != ErrorCode::kOk) return read;
      if (sides_[index].id.value != 0 && !IsTerminalOrderState(order.state)) {
        ++active_slots;
      }
    }
    const int written = std::fprintf(
        stdout,
        "bbo_market_making: stopped submitted=%" PRIu64
        " cancel_requests=%" PRIu64 " fills=%" PRIu64 " filled_qty=%" PRIu64
        " cash_balance=%" PRId64 " cash_reserved=%" PRId64 " total_qty=%" PRId64
        " available_qty=%" PRId64 " reserved_qty=%" PRId64 " slots=%u\n",
        submitted_, cancel_requests_, fills_, filled_qty_, cash.balance,
        cash.reserved, position.total_qty, position.available_qty,
        position.reserved_qty, active_slots);
    return written < 0 || std::fflush(stdout) != 0 ? ErrorCode::kIoError
                                                   : ErrorCode::kOk;
  }

 private:
  // Retry applies to submissions and cancels, independently for each side.
  struct SideState {
    StrategyOrderId id;
    std::uint64_t retry_not_before_ns = 0;
  };

  struct QuoteTarget {
    bool valid = false;
    std::int64_t bid_price = 0;
    std::int64_t ask_price = 0;
    std::optional<std::uint64_t> expiry;
  };

  static constexpr std::size_t SideIndex(OrderSide side) noexcept {
    return side == OrderSide::kBuy ? 0 : 1;
  }

  // Copies and validates account invariants without changing outputs on error.
  ErrorCode ReadAccount(const StrategyContext& context, CashSnapshot& cash,
                        PositionSnapshot& position) const noexcept {
    CashSnapshot candidate_cash;
    PositionSnapshot candidate_position;
    if (!context.trading().TryGetCash(candidate_cash) ||
        !context.trading().TryGetPosition(instrument_.id, candidate_position) ||
        candidate_cash.balance < 0 || candidate_cash.reserved < 0 ||
        candidate_cash.reserved > candidate_cash.balance ||
        candidate_cash.available !=
            candidate_cash.balance - candidate_cash.reserved ||
        candidate_position.instrument_id != instrument_.id ||
        candidate_position.total_qty < 0 ||
        candidate_position.available_qty < 0 ||
        candidate_position.reserved_qty < 0 ||
        candidate_position.reserved_qty > candidate_position.total_qty ||
        candidate_position.available_qty >
            candidate_position.total_qty - candidate_position.reserved_qty) {
      return ErrorCode::kCorrupt;
    }
    cash = candidate_cash;
    position = candidate_position;
    return ErrorCode::kOk;
  }

  // An empty slot produces a zero snapshot. Occupied slots must still identify
  // their own side/instrument; failure preserves out and all strategy state.
  ErrorCode ReadSlot(const StrategyContext& context, std::size_t index,
                     OrderSnapshot& out) const noexcept {
    const auto id = sides_[index].id;
    if (id.value == 0) {
      out = {};
      return ErrorCode::kOk;
    }
    OrderSnapshot order;
    const auto expected_side = index == 0 ? OrderSide::kBuy : OrderSide::kSell;
    if (!context.trading().TryGetOrder(id, order) ||
        order.strategy_order_id != id ||
        order.instrument_id != instrument_.id || order.side != expected_side ||
        order.limit_price <= 0 || order.original_qty != config_.quote_qty() ||
        order.cumulative_filled_qty < 0 ||
        order.cumulative_filled_qty > order.original_qty ||
        order.leaves_qty < 0 ||
        (IsTerminalOrderState(order.state)
             ? order.leaves_qty != 0
             : order.leaves_qty !=
                   order.original_qty - order.cumulative_filled_qty)) {
      return ErrorCode::kCorrupt;
    }
    out = order;
    return ErrorCode::kOk;
  }

  // Invalid/stale quotes yield no target. A future timestamp or unrepresentable
  // expiry violates the host's monotonic clock contract and returns an error.
  ErrorCode ReadTarget(const StrategyContext& context, std::uint64_t now,
                       QuoteTarget& out) const noexcept {
    out = {};
    QuoteTick quote;
    if (!context.market_data().TryGetLatestQuote(instrument_.id, quote)) {
      return ErrorCode::kOk;
    }
    if (quote.instrument_id != instrument_.id || quote.receive_time_ns > now) {
      return ErrorCode::kCorrupt;
    }
    if (!yquant::model::HasQuoteFlag(quote.flags, QuoteFlag::kBidValid) ||
        !yquant::model::HasQuoteFlag(quote.flags, QuoteFlag::kAskValid) ||
        !yquant::model::HasQuoteFlag(quote.flags, QuoteFlag::kTradable) ||
        quote.bid_price <= 0 || quote.ask_price <= quote.bid_price ||
        quote.bid_qty <= 0 || quote.ask_qty <= 0) {
      return ErrorCode::kOk;
    }
    if (config_.quote_timeout_ns() > kMaxCounter - quote.receive_time_ns) {
      return ErrorCode::kResourceExhausted;
    }
    const auto expiry = quote.receive_time_ns + config_.quote_timeout_ns();
    if (expiry <= now) return ErrorCode::kOk;
    out.valid = true;
    out.bid_price = quote.bid_price;
    out.ask_price = quote.ask_price;
    out.expiry = expiry;
    return ErrorCode::kOk;
  }

  // Fails before changing the deadline if monotonic addition would overflow.
  ErrorCode DelayRetry(SideState& side, std::uint64_t now) noexcept {
    if (config_.retry_delay_ns() > kMaxCounter - now) {
      return ErrorCode::kResourceExhausted;
    }
    side.retry_not_before_ns = now + config_.retry_delay_ns();
    return ErrorCode::kOk;
  }

  // Calculates the exact buy reservation in micro-yuan. Failure leaves out
  // unchanged and is fatal rather than submitting an unrepresentable amount.
  ErrorCode BuyCost(std::int64_t price, std::int64_t& out) const noexcept {
    if (price <= 0 || price > kMaxQuantity / config_.quote_qty()) {
      return ErrorCode::kInvalidArgument;
    }
    const auto amount = price * config_.quote_qty();
    if (amount > kMaxQuantity / price_scale_)
      return ErrorCode::kInvalidArgument;
    out = amount * price_scale_;
    return ErrorCode::kOk;
  }

  // Evaluates each side's worst-case exposure independently. Existing orders
  // are checked with their remaining quantity and ignore their own cash/share
  // reservations. New orders require a full fixed-size reservation.
  ErrorCode ReconcileSide(StrategyContext& context, std::size_t index,
                          const QuoteTarget& target,
                          std::uint64_t now) noexcept {
    auto& side = sides_[index];
    std::array<OrderSnapshot, 2> orders;
    for (std::size_t current = 0; current != sides_.size(); ++current) {
      const auto read = ReadSlot(context, current, orders[current]);
      if (read != ErrorCode::kOk) return read;
      if (sides_[current].id.value != 0 &&
          IsTerminalOrderState(orders[current].state)) {
        sides_[current].id = {};
        orders[current] = {};
      }
    }
    CashSnapshot cash;
    PositionSnapshot position;
    const auto account = ReadAccount(context, cash, position);
    if (account != ErrorCode::kOk) return account;
    const bool occupied = side.id.value != 0;
    const auto& order = orders[index];
    const auto& opposite = orders[1 - index];
    const bool opposite_live = sides_[1 - index].id.value != 0;
    const auto price = index == 0 ? target.bid_price : target.ask_price;
    bool wanted = target.valid;
    if (index == 0) {
      const auto leaves = sides_[0].id.value != 0 ? orders[0].leaves_qty : 0;
      if (leaves > kMaxQuantity - position.total_qty) {
        return ErrorCode::kCorrupt;
      }
      const auto exposure = position.total_qty + leaves;
      wanted = wanted && exposure <= config_.max_position();
      if (!occupied) {
        wanted = wanted && exposure <= config_.max_position() &&
                 config_.quote_qty() <= config_.max_position() - exposure;
      }
    } else {
      const auto leaves = sides_[1].id.value != 0 ? orders[1].leaves_qty : 0;
      if (leaves > position.total_qty) return ErrorCode::kCorrupt;
      const auto exposure = position.total_qty - leaves;
      wanted = wanted && exposure >= config_.min_position();
      if (!occupied) {
        wanted = wanted && exposure >= config_.min_position() &&
                 config_.quote_qty() <= exposure - config_.min_position();
      }
    }
    if (now < side.retry_not_before_ns) return ErrorCode::kOk;
    if (occupied) {
      if (order.state != OrderState::kWorking || order.cancel_pending ||
          (wanted && order.limit_price == price)) {
        return ErrorCode::kOk;
      }
      if (cancel_requests_ == kMaxCounter) return ErrorCode::kResourceExhausted;
      const auto canceled = context.trading().CancelOrder(side.id);
      if (canceled == ErrorCode::kResourceExhausted)
        return DelayRetry(side, now);
      if (canceled != ErrorCode::kOk) return canceled;
      ++cancel_requests_;
      return ErrorCode::kOk;
    }
    if (!wanted ||
        (opposite_live && (index == 0 ? price >= opposite.limit_price
                                      : price <= opposite.limit_price))) {
      return ErrorCode::kOk;
    }
    if (index == 0) {
      std::int64_t cost = 0;
      const auto calculated = BuyCost(price, cost);
      if (calculated != ErrorCode::kOk) return calculated;
      if (cash.available < cost) return ErrorCode::kOk;
    } else if (position.available_qty < config_.quote_qty()) {
      return ErrorCode::kOk;
    }
    if (submitted_ == kMaxCounter) return ErrorCode::kResourceExhausted;
    OrderRequest request;
    request.instrument_id = instrument_.id;
    request.side = index == 0 ? OrderSide::kBuy : OrderSide::kSell;
    request.limit_price = price;
    request.quantity = config_.quote_qty();
    const auto submitted = context.trading().SubmitOrder(request, side.id);
    if (submitted == ErrorCode::kResourceExhausted)
      return DelayRetry(side, now);
    if (submitted != ErrorCode::kOk) return submitted;
    ++submitted_;
    return ErrorCode::kOk;
  }

  // Arms only future deadlines, so a stale quote without a pending retry does
  // not continuously rearm an already-due one-shot. Failure preserves service
  // state according to Timer's contract and terminates the strategy.
  ErrorCode ScheduleTimer(StrategyContext& context, const QuoteTarget& target,
                          std::uint64_t now) const noexcept {
    auto next = target.expiry;
    for (const auto& side : sides_) {
      if (side.retry_not_before_ns > now &&
          (!next || side.retry_not_before_ns < *next)) {
        next = side.retry_not_before_ns;
      }
    }
    if (next == context.timer().deadline()) return ErrorCode::kOk;
    return next ? context.timer().Arm(*next) : context.timer().Cancel();
  }

  // Callback-only reconciliation reads the latest cache and committed account,
  // then queues requests without synchronous execution reports or allocation.
  ErrorCode Reconcile(StrategyContext& context) noexcept {
    if (!started_) return ErrorCode::kInvalidState;
    if (!context.trading().accepting_orders()) return ErrorCode::kOk;
    const auto now = context.Now();
    QuoteTarget target;
    const auto read = ReadTarget(context, now, target);
    if (read != ErrorCode::kOk) return read;
    for (std::size_t index = 0; index != sides_.size(); ++index) {
      const auto reconciled = ReconcileSide(context, index, target, now);
      if (reconciled != ErrorCode::kOk) return reconciled;
    }
    const auto scheduled = ScheduleTimer(context, target, now);
    if (scheduled != ErrorCode::kOk) return scheduled;
    if (!first_submit_reported_ && submitted_ != 0) {
      first_submit_reported_ = true;
      return WriteMarker("bbo_market_making: first quotes submitted\n");
    }
    return ErrorCode::kOk;
  }

  BboMarketMakingConfig config_;
  InstrumentDefinition instrument_;
  std::array<SideState, 2> sides_{};
  std::int64_t price_scale_ = 1;
  std::uint64_t submitted_ = 0;
  std::uint64_t cancel_requests_ = 0;
  std::uint64_t fills_ = 0;
  std::uint64_t filled_qty_ = 0;
  bool started_ = false;
  bool first_submit_reported_ = false;
};

// Factory output changes only when allocation succeeds; the host owns the
// returned object and releases it through this module's paired destroy hook.
ErrorCode CreateStrategy(Strategy*& output) noexcept {
  auto* strategy = new (std::nothrow) BboMarketMakingStrategy;
  if (strategy == nullptr) return ErrorCode::kResourceExhausted;
  output = strategy;
  return ErrorCode::kOk;
}

void DestroyStrategy(Strategy* strategy) noexcept { delete strategy; }

const StrategyPlugin kPlugin = {
    yquant::strategy::kStrategyPluginAbiVersion,
    static_cast<std::uint32_t>(sizeof(StrategyPlugin)), CreateStrategy,
    DestroyStrategy};

}  // namespace

extern "C" YQUANT_STRATEGY_EXPORT const StrategyPlugin*
YQuantGetStrategyPlugin() noexcept {
  return &kPlugin;
}
