#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <string_view>

#include "simdjson.h"
#include "yquant/base/format.h"
#include "yquant/model/futures.h"
#include "yquant/model/instrument.h"
#include "yquant/model/order.h"
#include "yquant/model/quote_tick.h"
#include "yquant/model/trading_event.h"
#include "yquant/strategy/strategy.h"
#include "yquant/strategy/strategy_plugin.h"
#include "yquant/strategy/timer.h"

namespace {

using yquant::base::ErrorCode;
using yquant::model::ClientOrderId;
using yquant::model::Fill;
using yquant::model::FuturesAccountSnapshot;
using yquant::model::FuturesPositionBucket;
using yquant::model::FuturesPositionSnapshot;
using yquant::model::InstrumentDefinition;
using yquant::model::InstrumentId;
using yquant::model::IsTerminalOrderState;
using yquant::model::OrderRequest;
using yquant::model::OrderSide;
using yquant::model::OrderSnapshot;
using yquant::model::OrderState;
using yquant::model::OrderUpdate;
using yquant::model::OrderUpdateKind;
using yquant::model::PositionEffect;
using yquant::model::QuoteFlag;
using yquant::model::QuoteTick;
using yquant::strategy::ConfigError;
using yquant::strategy::Strategy;
using yquant::strategy::StrategyConfig;
using yquant::strategy::StrategyContext;
using yquant::strategy::StrategyPlugin;
using yquant::strategy::TimerEvent;

constexpr uint64_t kNsPerMs = 1000000;
constexpr int64_t kMaxQuantity = std::numeric_limits<int64_t>::max();
constexpr uint64_t kMaxCounter = std::numeric_limits<uint64_t>::max();

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
      text.remove_prefix(static_cast<size_t>(written));
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
class FuturesBboConfig final : public StrategyConfig {
 public:
  std::string_view venue() const noexcept { return {venue_, venue_size_}; }
  std::string_view symbol() const noexcept { return {symbol_, symbol_size_}; }
  int64_t quote_qty() const noexcept { return quote_qty_; }
  int64_t min_position() const noexcept { return min_position_; }
  int64_t max_position() const noexcept { return max_position_; }
  int64_t max_gross_position() const noexcept { return max_gross_position_; }
  uint64_t quote_timeout_ns() const noexcept { return quote_timeout_ns_; }
  uint64_t retry_delay_ns() const noexcept { return retry_delay_ns_; }

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

    constexpr std::array<std::string_view, 8> kFields = {
        "venue",          "symbol",
        "quote_qty",      "min_position",
        "max_position",   "quote_timeout_ms",
        "retry_delay_ms", "max_gross_position"};
    std::array<bool, kFields.size()> seen{};
    std::string_view venue;
    std::string_view symbol;
    int64_t quote_qty = 0;
    int64_t min_position = 0;
    int64_t max_position = 0;
    int64_t max_gross = 0;
    uint64_t timeout_ms = 0;
    uint64_t retry_ms = 0;
    for (const auto field : object) {
      size_t index = 0;
      while (index != kFields.size() && field.key != kFields[index]) {
        ++index;
      }
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
        case 7:
          result = field.value.get_int64().get(max_gross);
          break;
      }
      if (result != simdjson::SUCCESS) {
        return ParseError(result, field.key, error);
      }
    }
    for (size_t index = 0; index != kFields.size(); ++index) {
      if (!seen[index]) {
        error.Set(kFields[index], "required parameter is missing");
        return ErrorCode::kInvalidArgument;
      }
    }
    if (venue != "SHFE" && venue != "DCE" && venue != "CZCE" &&
        venue != "CFFEX" && venue != "INE" && venue != "GFEX") {
      error.Set("venue", "must identify a supported futures venue");
      return ErrorCode::kInvalidArgument;
    }
    InstrumentId id;
    if (yquant::model::MakeInstrumentId(venue, symbol, id) != ErrorCode::kOk) {
      error.Set("symbol", "must be a canonical futures contract symbol");
      return ErrorCode::kInvalidArgument;
    }
    if (quote_qty <= 0) {
      error.Set("quote_qty", "must be a positive integer number of lots");
      return ErrorCode::kInvalidArgument;
    }
    if (max_gross <= 0 || quote_qty > max_gross || min_position > 0 ||
        max_position < 0 || min_position < -max_gross ||
        max_position > max_gross || min_position > max_position - quote_qty) {
      error.Set("max_gross_position",
                "invalid net/gross bounds or quote quantity");
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
    symbol_size_ = symbol.size();
    quote_qty_ = quote_qty;
    min_position_ = min_position;
    max_position_ = max_position;
    max_gross_position_ = max_gross;
    quote_timeout_ns_ = timeout_ms * kNsPerMs;
    retry_delay_ns_ = retry_ms * kNsPerMs;
    return ErrorCode::kOk;
  }

 private:
  char venue_[8] = {};
  size_t venue_size_ = 0;
  char symbol_[16] = {};
  size_t symbol_size_ = 0;
  int64_t quote_qty_ = 0;
  int64_t min_position_ = 0;
  int64_t max_position_ = 0;
  int64_t max_gross_position_ = 0;
  uint64_t quote_timeout_ns_ = 0;
  uint64_t retry_delay_ns_ = 0;
};

// Two live side slots with explicit close/open effects. The host owns FIFO
// positions and margin; this strategy never interprets net-zero as flat.
class FuturesBboStrategy final : public Strategy {
 public:
  FuturesBboConfig& config() noexcept override { return config_; }

  ErrorCode OnStart(StrategyContext& context) noexcept override {
    if (!config_.ready() || started_ || context.trading().accepting_orders()) {
      return ErrorCode::kInvalidState;
    }
    const auto found = context.market_data().FindInstrument(
        config_.venue(), config_.symbol(), instrument_);
    if (found != ErrorCode::kOk) {
      return found;
    }
    if (instrument_.type != yquant::model::InstrumentType::kFutures ||
        instrument_.quantity_step <= 0 ||
        config_.quote_qty() % instrument_.quantity_step != 0) {
      return ErrorCode::kInvalidArgument;
    }
    broker_managed_ = context.trading().supports_generic_close();
    generic_close_ = broker_managed_ &&
                     instrument_.venue != yquant::model::Venue::kShfe &&
                     instrument_.venue != yquant::model::Venue::kIne;
    FuturesAccountSnapshot account;
    FuturesPositionSnapshot position;
    int64_t gross = 0;
    const auto read = ReadAccount(context, account, position, gross);
    if (read != ErrorCode::kOk) {
      return read;
    }
    if (position.net_qty() < config_.min_position() ||
        position.net_qty() > config_.max_position() ||
        gross > config_.max_gross_position() || account.reserved != 0) {
      return ErrorCode::kInvalidArgument;
    }
    const auto subscribed =
        context.market_data().SubscribeQuotes(instrument_.id);
    if (subscribed != ErrorCode::kOk) {
      return subscribed;
    }
    max_gross_seen_ = gross;
    had_inventory_ = gross != 0;
    started_ = true;
    const int written =
        ::yquant::base::Print(stdout, "futures_bbo: ready net={} gross={}\n",
                              position.net_qty(), gross);
    return written < 0 || std::fflush(stdout) != 0 ? ErrorCode::kIoError
                                                   : ErrorCode::kOk;
  }
  ErrorCode OnTick(StrategyContext& context,
                   const QuoteTick& quote) noexcept override {
    if (quote.instrument_id != instrument_.id) {
      return ErrorCode::kCorrupt;
    }
    return Reconcile(context);
  }
  ErrorCode OnOrderUpdate(StrategyContext& context,
                          const OrderUpdate& update) noexcept override {
    OrderSnapshot order;
    if (!context.trading().TryGetOrder(update.order.client_order_id, order) ||
        order != update.order || order.instrument_id != instrument_.id ||
        (order.side != OrderSide::kBuy && order.side != OrderSide::kSell)) {
      return ErrorCode::kCorrupt;
    }
    auto& side = sides_[order.side == OrderSide::kBuy ? 0 : 1];
    const bool matched = side.id == order.client_order_id;
    if (matched && IsTerminalOrderState(order.state)) {
      side.id = {};
    }
    const auto observed = ObserveInventory(context);
    if (observed != ErrorCode::kOk) {
      return observed;
    }
    if (!context.trading().accepting_orders()) {
      return ErrorCode::kOk;
    }
    if (matched && (update.kind == OrderUpdateKind::kRejected ||
                    update.kind == OrderUpdateKind::kCancelRejected)) {
      const auto delayed = Delay(side, context.Now());
      if (delayed != ErrorCode::kOk) {
        return delayed;
      }
    }
    return Reconcile(context);
  }
  ErrorCode OnFill(StrategyContext& context,
                   const Fill& fill) noexcept override {
    OrderSnapshot order;
    if (fill.instrument_id != instrument_.id || fill.quantity <= 0 ||
        !context.trading().TryGetOrder(fill.client_order_id, order) ||
        order.side != fill.side ||
        order.position_effect != fill.position_effect ||
        order.broker_order_id != fill.broker_order_id) {
      return ErrorCode::kCorrupt;
    }
    if (fills_ == kMaxCounter ||
        static_cast<uint64_t>(fill.quantity) > kMaxCounter - filled_qty_) {
      return ErrorCode::kResourceExhausted;
    }
    ++fills_;
    filled_qty_ += static_cast<uint64_t>(fill.quantity);
    const auto observed = ObserveInventory(context);
    if (observed != ErrorCode::kOk) {
      return observed;
    }
    if (fills_ == 1) {
      return WriteMarker("futures_bbo: first fill received\n");
    }
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
    FuturesAccountSnapshot account;
    FuturesPositionSnapshot position;
    int64_t gross = 0;
    const auto read = ReadAccount(context, account, position, gross);
    if (read != ErrorCode::kOk) {
      return read;
    }
    unsigned slots = 0;
    for (size_t index = 0; index < 2; ++index) {
      OrderSnapshot order;
      if (ReadSlot(context, index, order) != ErrorCode::kOk) {
        return ErrorCode::kCorrupt;
      }
      if (sides_[index].id.value != 0 && !IsTerminalOrderState(order.state)) {
        ++slots;
      }
    }
    const int written = ::yquant::base::Print(
        stdout,
        "futures_bbo: stopped submitted={} cancel_requests={} fills={} "
        "filled_qty={} net={} gross={} long_today={} long_yesterday={} "
        "short_today={} short_yesterday={} balance={} equity={} available={} "
        "margin={} reserved={} realized_pnl={} unrealized_pnl={} commission={} "
        "frozen_qty={} slots={} max_gross_seen={}\n",
        submitted_, cancels_, fills_, filled_qty_, position.net_qty(), gross,
        position.long_today.quantity, position.long_yesterday.quantity,
        position.short_today.quantity, position.short_yesterday.quantity,
        account.balance, account.equity, account.available, account.margin,
        account.reserved, account.realized_pnl, account.unrealized_pnl,
        account.commission,
        position.long_today.frozen_qty + position.long_yesterday.frozen_qty +
            position.short_today.frozen_qty +
            position.short_yesterday.frozen_qty +
            position.unallocated_long_frozen +
            position.unallocated_short_frozen,
        slots, max_gross_seen_);
    return written < 0 || std::fflush(stdout) != 0 ? ErrorCode::kIoError
                                                   : ErrorCode::kOk;
  }

 private:
  struct Side {
    ClientOrderId id;
    uint64_t retry = 0;
    // Retained after terminal orders to track submission of both directions.
    bool ever_submitted = false;
  };
  struct Target {
    bool valid = false;
    int64_t bid = 0;
    int64_t ask = 0;
    std::optional<uint64_t> expiry;
  };
  struct Intent {
    PositionEffect effect = PositionEffect::kNone;
    int64_t quantity = 0;
  };

  // Reads copied host snapshots and validates representable account/gross
  // arithmetic. On error outputs can contain unvalidated copies; callers use
  // them only on kOk. No host ledger or strategy request slot is changed.
  ErrorCode ReadAccount(const StrategyContext& context,
                        FuturesAccountSnapshot& account,
                        FuturesPositionSnapshot& position,
                        int64_t& gross) const noexcept {
    if (!context.trading().TryGetFuturesAccount(account) ||
        !context.trading().TryGetFuturesPosition(instrument_.id, position) ||
        position.instrument_id != instrument_.id || account.margin < 0 ||
        account.reserved < 0 || position.margin < 0) {
      return ErrorCode::kCorrupt;
    }
    gross = 0;
    const std::array<FuturesPositionBucket, 4> buckets = {
        position.long_today, position.long_yesterday, position.short_today,
        position.short_yesterday};
    for (const auto& bucket : buckets) {
      if (bucket.quantity < 0 || bucket.frozen_qty < 0 ||
          bucket.frozen_qty > bucket.quantity || bucket.open_notional < 0 ||
          bucket.quantity > kMaxQuantity - gross) {
        return ErrorCode::kCorrupt;
      }
      gross += bucket.quantity;
    }
    if (position.unallocated_long_frozen < 0 ||
        position.unallocated_short_frozen < 0 ||
        position.unallocated_long_frozen >
            position.long_today.quantity + position.long_yesterday.quantity -
                position.long_today.frozen_qty -
                position.long_yesterday.frozen_qty ||
        position.unallocated_short_frozen >
            position.short_today.quantity + position.short_yesterday.quantity -
                position.short_today.frozen_qty -
                position.short_yesterday.frozen_qty) {
      return ErrorCode::kCorrupt;
    }
    constexpr auto kMinimum = std::numeric_limits<int64_t>::min();
    if (!context.trading().supports_generic_close() &&
        (account.equity < kMinimum + account.margin ||
         account.equity - account.margin < kMinimum + account.reserved ||
         account.available !=
             account.equity - account.margin - account.reserved)) {
      return ErrorCode::kCorrupt;
    }
    return ErrorCode::kOk;
  }
  // Records committed exposure and one-time markers without issuing requests.
  // Marker I/O failure retains observed counters and propagates to host
  // draining.
  ErrorCode ObserveInventory(const StrategyContext& context) noexcept {
    FuturesAccountSnapshot account;
    FuturesPositionSnapshot position;
    int64_t gross = 0;
    const auto read = ReadAccount(context, account, position, gross);
    if (read != ErrorCode::kOk) {
      return read;
    }
    if (gross > max_gross_seen_) {
      max_gross_seen_ = gross;
    }
    if (gross != 0) {
      had_inventory_ = true;
    }
    const bool locked =
        position.long_today.quantity + position.long_yesterday.quantity != 0 &&
        position.short_today.quantity + position.short_yesterday.quantity != 0;
    if (locked && !locked_seen_) {
      locked_seen_ = true;
      const auto printed =
          WriteMarker("futures_bbo: locked inventory observed\n");
      if (printed != ErrorCode::kOk) {
        return printed;
      }
    }
    if (had_inventory_ && gross == 0 && !round_trip_reported_) {
      round_trip_reported_ = true;
      return WriteMarker("futures_bbo: round trip completed\n");
    }
    return ErrorCode::kOk;
  }
  // Empty IDs produce zero snapshots. Occupied IDs must identify this side and
  // instrument. Failure may overwrite order but never changes the slot itself.
  ErrorCode ReadSlot(const StrategyContext& context, size_t index,
                     OrderSnapshot& order) const noexcept {
    if (sides_[index].id.value == 0) {
      order = {};
      return ErrorCode::kOk;
    }
    if (!context.trading().TryGetOrder(sides_[index].id, order) ||
        order.instrument_id != instrument_.id ||
        order.client_order_id != sides_[index].id ||
        order.side != (index == 0 ? OrderSide::kBuy : OrderSide::kSell) ||
        order.original_qty <= 0 || order.original_qty > config_.quote_qty() ||
        order.leaves_qty < 0 || order.leaves_qty > order.original_qty ||
        (order.position_effect != PositionEffect::kOpen &&
         order.position_effect != PositionEffect::kCloseToday &&
         order.position_effect != PositionEffect::kCloseYesterday &&
         !(generic_close_ &&
           order.position_effect == PositionEffect::kClose))) {
      return ErrorCode::kCorrupt;
    }
    return ErrorCode::kOk;
  }
  // Called with an empty target: absent, invalid or stale BBO leaves it empty.
  // Clock/expiry errors propagate without changing requests or timer state.
  ErrorCode ReadTarget(const StrategyContext& context, uint64_t now,
                       Target& target) const noexcept {
    QuoteTick quote;
    if (!context.market_data().TryGetLatestQuote(instrument_.id, quote)) {
      return ErrorCode::kOk;
    }
    if (quote.receive_time_ns > now ||
        config_.quote_timeout_ns() > kMaxCounter - quote.receive_time_ns) {
      return ErrorCode::kCorrupt;
    }
    if (!yquant::model::HasQuoteFlag(quote.flags, QuoteFlag::kBidValid) ||
        !yquant::model::HasQuoteFlag(quote.flags, QuoteFlag::kAskValid) ||
        !yquant::model::HasQuoteFlag(quote.flags, QuoteFlag::kTradable) ||
        quote.bid_price <= 0 || quote.ask_price <= quote.bid_price ||
        quote.bid_qty <= 0 || quote.ask_qty <= 0) {
      return ErrorCode::kOk;
    }
    const auto expiry = quote.receive_time_ns + config_.quote_timeout_ns();
    if (expiry <= now) {
      return ErrorCode::kOk;
    }
    target = {true, quote.bid_price, quote.ask_price, expiry};
    return ErrorCode::kOk;
  }
  // Replaces one retry deadline; overflow preserves the previous value.
  ErrorCode Delay(Side& side, uint64_t now) noexcept {
    if (config_.retry_delay_ns() > kMaxCounter - now) {
      return ErrorCode::kResourceExhausted;
    }
    side.retry = now + config_.retry_delay_ns();
    return ErrorCode::kOk;
  }
  // Restores the current order's own freeze before selecting a close bucket.
  // Any opposite inventory blocks opening more gross exposure on this side.
  // Called with empty intent; failure preserves it and all host reservations.
  ErrorCode SelectIntent(size_t side, const FuturesPositionSnapshot& position,
                         const OrderSnapshot& own, bool occupied,
                         Intent& intent) const noexcept {
    const auto& today = side == 0 ? position.short_today : position.long_today;
    const auto& yesterday =
        side == 0 ? position.short_yesterday : position.long_yesterday;
    auto today_available = today.quantity - today.frozen_qty;
    auto yesterday_available = yesterday.quantity - yesterday.frozen_qty;
    const auto unallocated = side == 0 ? position.unallocated_short_frozen
                                       : position.unallocated_long_frozen;
    if (generic_close_) {
      auto available = today_available + yesterday_available - unallocated;
      if (occupied && own.position_effect == PositionEffect::kClose) {
        available +=
            std::min(own.leaves_qty,
                     today.frozen_qty + yesterday.frozen_qty + unallocated);
      }
      if (available > 0) {
        intent = {PositionEffect::kClose,
                  std::min(available, config_.quote_qty())};
      } else if (today.quantity == 0 && yesterday.quantity == 0) {
        intent = {PositionEffect::kOpen, config_.quote_qty()};
      }
      return ErrorCode::kOk;
    }
    if (unallocated != 0) {
      return ErrorCode::kUnsupported;
    }
    if (occupied && own.position_effect == PositionEffect::kCloseToday) {
      if (!broker_managed_ && own.leaves_qty > today.frozen_qty) {
        return ErrorCode::kCorrupt;
      }
      today_available += broker_managed_
                             ? std::min(own.leaves_qty, today.frozen_qty)
                             : own.leaves_qty;
    } else if (occupied &&
               own.position_effect == PositionEffect::kCloseYesterday) {
      if (!broker_managed_ && own.leaves_qty > yesterday.frozen_qty) {
        return ErrorCode::kCorrupt;
      }
      yesterday_available +=
          broker_managed_ ? std::min(own.leaves_qty, yesterday.frozen_qty)
                          : own.leaves_qty;
    }
    if (today_available > 0) {
      intent = {PositionEffect::kCloseToday,
                std::min(today_available, config_.quote_qty())};
    } else if (yesterday_available > 0) {
      intent = {PositionEffect::kCloseYesterday,
                std::min(yesterday_available, config_.quote_qty())};
    } else if (today.quantity == 0 && yesterday.quantity == 0) {
      intent = {PositionEffect::kOpen, config_.quote_qty()};
    }
    return ErrorCode::kOk;
  }
  // Returns a saturated nonnegative distance; comparisons remain safe even
  // when a configured net range spans more than INT64_MAX units.
  static int64_t Distance(int64_t high, int64_t low) noexcept {
    if (low < 0 && high > kMaxQuantity + low) {
      return kMaxQuantity;
    }
    return high - low;
  }
  // Queues at most one local action for this side, retaining pending requests.
  // Earlier accepted requests remain committed if a later check fails; host
  // error handling drains them after this callback returns.
  ErrorCode ReconcileSide(StrategyContext& context, size_t index,
                          const Target& target, uint64_t now) noexcept {
    std::array<OrderSnapshot, 2> orders;
    for (size_t side = 0; side < 2; ++side) {
      const auto read = ReadSlot(context, side, orders[side]);
      if (read != ErrorCode::kOk) {
        return read;
      }
      if (sides_[side].id.value != 0 &&
          IsTerminalOrderState(orders[side].state)) {
        sides_[side].id = {};
        orders[side] = {};
      }
    }
    auto& side = sides_[index];
    const bool occupied = side.id.value != 0;
    const auto& order = orders[index];
    // A local PendingNew has no confirmed gateway reservation yet. Retain
    // pending requests until their order/account report arrives.
    if (occupied &&
        (order.state != OrderState::kWorking || order.cancel_pending)) {
      return ErrorCode::kOk;
    }
    FuturesAccountSnapshot account;
    FuturesPositionSnapshot position;
    int64_t gross = 0;
    const auto account_read = ReadAccount(context, account, position, gross);
    if (account_read != ErrorCode::kOk) {
      return account_read;
    }
    if (broker_managed_) {
      // A batched account query may already include executions whose individual
      // reports have not reached this callback. Retain both slots until those
      // closing reports settle; do not reopen from that leading account view.
      for (size_t current = 0; current < 2; ++current) {
        const auto& closing = orders[current];
        if (sides_[current].id.value == 0 ||
            closing.position_effect == PositionEffect::kOpen) {
          continue;
        }
        const auto& today =
            current == 0 ? position.short_today : position.long_today;
        const auto& yesterday =
            current == 0 ? position.short_yesterday : position.long_yesterday;
        const auto held =
            closing.position_effect == PositionEffect::kClose
                ? today.quantity + yesterday.quantity
            : closing.position_effect == PositionEffect::kCloseToday
                ? today.quantity
                : yesterday.quantity;
        if (held < closing.leaves_qty) {
          return ErrorCode::kOk;
        }
      }
    }
    Intent intent;
    const auto selected =
        SelectIntent(index, position, order, occupied, intent);
    if (selected != ErrorCode::kOk) {
      return selected;
    }
    bool wanted = target.valid && intent.quantity > 0;
    const auto net = position.net_qty();
    const auto leaves = sides_[index].id.value == 0 ? 0 : order.leaves_qty;
    if (index == 0) {
      if (net > config_.max_position()) {
        wanted = false;
      } else {
        const auto room = Distance(config_.max_position(), net);
        wanted = wanted && leaves <= room &&
                 (occupied || intent.quantity <= room - leaves);
      }
    } else {
      if (net < config_.min_position()) {
        wanted = false;
      } else {
        const auto room = Distance(net, config_.min_position());
        wanted = wanted && leaves <= room &&
                 (occupied || intent.quantity <= room - leaves);
      }
    }
    int64_t open_leaves = 0;
    for (size_t current = 0; current < 2; ++current) {
      if (sides_[current].id.value != 0 &&
          orders[current].position_effect == PositionEffect::kOpen) {
        if (orders[current].leaves_qty > kMaxQuantity - open_leaves) {
          return ErrorCode::kCorrupt;
        }
        open_leaves += orders[current].leaves_qty;
      }
    }
    if (intent.effect == PositionEffect::kOpen) {
      wanted = wanted && gross <= config_.max_gross_position() &&
               open_leaves <= config_.max_gross_position() - gross;
      if (!occupied && wanted) {
        wanted = intent.quantity <=
                 config_.max_gross_position() - gross - open_leaves;
      }
      if (account.available < 0) {
        wanted = false;
      }
    }
    const auto price = index == 0 ? target.bid : target.ask;
    if (now < side.retry) {
      return ErrorCode::kOk;
    }
    if (occupied) {
      if (order.state != OrderState::kWorking || order.cancel_pending ||
          (wanted && order.limit_price == price &&
           order.position_effect == intent.effect)) {
        return ErrorCode::kOk;
      }
      if (cancels_ == kMaxCounter) {
        return ErrorCode::kResourceExhausted;
      }
      const auto canceled = context.trading().CancelOrder(side.id);
      if (canceled == ErrorCode::kResourceExhausted) {
        return Delay(side, now);
      }
      if (canceled != ErrorCode::kOk) {
        return canceled;
      }
      ++cancels_;
      return ErrorCode::kOk;
    }
    const auto& opposite = orders[1 - index];
    if (!wanted || (sides_[1 - index].id.value != 0 &&
                    (index == 0 ? price >= opposite.limit_price
                                : price <= opposite.limit_price))) {
      return ErrorCode::kOk;
    }
    if (submitted_ == kMaxCounter) {
      return ErrorCode::kResourceExhausted;
    }
    OrderRequest request;
    request.instrument_id = instrument_.id;
    request.side = index == 0 ? OrderSide::kBuy : OrderSide::kSell;
    request.limit_price = price;
    request.quantity = intent.quantity;
    request.position_effect = intent.effect;
    const auto submitted = context.trading().SubmitOrder(request, side.id);
    if (submitted == ErrorCode::kResourceExhausted) {
      return Delay(side, now);
    }
    if (submitted != ErrorCode::kOk) {
      return submitted;
    }
    ++submitted_;
    side.ever_submitted = true;
    return ErrorCode::kOk;
  }
  // Callback-only coordination; no borrowed service or event reference escapes.
  // Updates both slots, then the earliest expiry/retry timer. Errors retain
  // already accepted local requests and propagate to ordinary host draining.
  ErrorCode Reconcile(StrategyContext& context) noexcept {
    if (!started_) {
      return ErrorCode::kInvalidState;
    }
    if (!context.trading().accepting_orders()) {
      return ErrorCode::kOk;
    }
    const auto now = context.Now();
    Target target;
    const auto read = ReadTarget(context, now, target);
    if (read != ErrorCode::kOk) {
      return read;
    }
    for (size_t side = 0; side < 2; ++side) {
      const auto result = ReconcileSide(context, side, target, now);
      if (result != ErrorCode::kOk) {
        return result;
      }
    }
    auto deadline = target.expiry;
    for (const auto& side : sides_) {
      if (side.retry > now && (!deadline || side.retry < *deadline)) {
        deadline = side.retry;
      }
    }
    if (deadline != context.timer().deadline()) {
      const auto changed =
          deadline ? context.timer().Arm(*deadline) : context.timer().Cancel();
      if (changed != ErrorCode::kOk) {
        return changed;
      }
    }
    if (!first_submit_reported_ && submitted_ != 0) {
      first_submit_reported_ = true;
      const auto written = WriteMarker("futures_bbo: first quotes submitted\n");
      if (written != ErrorCode::kOk) {
        return written;
      }
    }
    if (!both_sides_reported_ && sides_[0].ever_submitted &&
        sides_[1].ever_submitted) {
      both_sides_reported_ = true;
      return WriteMarker("futures_bbo: both sides submitted\n");
    }
    return ErrorCode::kOk;
  }

  FuturesBboConfig config_;
  InstrumentDefinition instrument_;
  std::array<Side, 2> sides_{};
  uint64_t submitted_ = 0;
  uint64_t cancels_ = 0;
  uint64_t fills_ = 0;
  uint64_t filled_qty_ = 0;
  int64_t max_gross_seen_ = 0;
  bool started_ = false;
  bool generic_close_ = false;
  bool broker_managed_ = false;
  bool first_submit_reported_ = false;
  bool both_sides_reported_ = false;
  bool had_inventory_ = false;
  bool locked_seen_ = false;
  bool round_trip_reported_ = false;
};

// Allocation failure leaves out null; an unstarted instance is safe to destroy.
ErrorCode CreateStrategy(Strategy*& out) noexcept {
  out = new (std::nothrow) FuturesBboStrategy;
  return out != nullptr ? ErrorCode::kOk : ErrorCode::kResourceExhausted;
}
void DestroyStrategy(Strategy* strategy) noexcept { delete strategy; }
const StrategyPlugin kPlugin = {yquant::strategy::kStrategyPluginAbiVersion,
                                static_cast<uint32_t>(sizeof(StrategyPlugin)),
                                CreateStrategy, DestroyStrategy};

}  // namespace

extern "C" YQUANT_STRATEGY_EXPORT const StrategyPlugin*
YQuantGetStrategyPlugin() noexcept {
  return &kPlugin;
}
