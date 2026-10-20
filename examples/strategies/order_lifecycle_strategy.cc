#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>

#include "simdjson.h"
#include "yquant/model/account.h"
#include "yquant/model/order.h"
#include "yquant/model/trading_event.h"
#include "yquant/strategy/strategy.h"
#include "yquant/strategy/strategy_plugin.h"

namespace {

using yquant::base::ErrorCode;
using yquant::model::CashSnapshot;
using yquant::model::ClientOrderId;
using yquant::model::Fill;
using yquant::model::InstrumentDefinition;
using yquant::model::OrderRequest;
using yquant::model::OrderSide;
using yquant::model::OrderSnapshot;
using yquant::model::OrderState;
using yquant::model::OrderUpdate;
using yquant::model::OrderUpdateKind;
using yquant::model::PositionSnapshot;
using yquant::model::QuoteFlag;
using yquant::model::QuoteTick;
using yquant::model::SourceId;
using yquant::strategy::ConfigError;
using yquant::strategy::Strategy;
using yquant::strategy::StrategyConfig;
using yquant::strategy::StrategyContext;
using yquant::strategy::StrategyPlugin;

// Owns the instrument selection and quantity; the parser/input die at startup.
class OrderLifecycleConfig final : public StrategyConfig {
 public:
  SourceId source_id() const noexcept { return source_id_; }
  std::string_view venue() const noexcept { return {venue_, venue_size_}; }
  std::string_view symbol() const noexcept {
    return {symbol_, sizeof(symbol_)};
  }
  std::int64_t quantity() const noexcept { return quantity_; }

 protected:
  ErrorCode LoadParameters(std::string_view json,
                           ConfigError& error) noexcept override {
    simdjson::dom::parser parser;
    simdjson::dom::object object;
    const auto parsed =
        parser.parse(json.data(), json.size()).get_object().get(object);
    if (parsed != simdjson::SUCCESS) {
      error.Set("", simdjson::error_message(parsed));
      return parsed == simdjson::MEMALLOC ? ErrorCode::kResourceExhausted
                                          : ErrorCode::kInvalidArgument;
    }
    std::uint64_t source_id = 0;
    std::string_view venue;
    std::string_view symbol;
    std::int64_t quantity = 0;
    for (const auto field : object) {
      bool invalid = false;
      if (field.key == "source_id") {
        invalid = field.value.get_uint64().get(source_id) || source_id == 0 ||
                  source_id > std::numeric_limits<std::uint32_t>::max();
      } else if (field.key == "venue") {
        invalid = field.value.get_string().get(venue) ||
                  (venue != "SSE" && venue != "SZSE" && venue != "BSE");
      } else if (field.key == "symbol") {
        invalid =
            field.value.get_string().get(symbol) || symbol.size() != 6 ||
            symbol.find_first_not_of("0123456789") != std::string_view::npos;
      } else if (field.key == "quantity") {
        invalid = field.value.get_int64().get(quantity) || quantity <= 0;
      } else {
        error.Set(field.key, "unknown parameter");
        return ErrorCode::kInvalidArgument;
      }
      if (invalid) {
        error.Set(field.key, "invalid parameter value or type");
        return ErrorCode::kInvalidArgument;
      }
    }
    if (source_id == 0 || venue.empty() || symbol.empty() || quantity == 0) {
      error.Set(source_id == 0   ? "source_id"
                : venue.empty()  ? "venue"
                : symbol.empty() ? "symbol"
                                 : "quantity",
                "required parameter is missing");
      return ErrorCode::kInvalidArgument;
    }
    source_id_.value = static_cast<std::uint32_t>(source_id);
    std::memcpy(venue_, venue.data(), venue.size());
    venue_size_ = venue.size();
    std::memcpy(symbol_, symbol.data(), symbol.size());
    quantity_ = quantity;
    return ErrorCode::kOk;
  }

 private:
  SourceId source_id_;
  char venue_[4] = {};
  std::size_t venue_size_ = 0;
  char symbol_[6] = {};
  std::int64_t quantity_ = 0;
};

const char* StateName(OrderState state) noexcept {
  switch (state) {
    case OrderState::kPendingNew:
      return "pending_new";
    case OrderState::kWorking:
      return "working";
    case OrderState::kFilled:
      return "filled";
    case OrderState::kCanceled:
      return "canceled";
    case OrderState::kRejected:
      return "rejected";
  }
  return "unknown";
}

// Submits once from the first tradable ask and checks every subsequent fact.
// It stores snapshots/counters only and remains running after order completion.
class OrderLifecycleStrategy final : public Strategy {
 public:
  OrderLifecycleConfig& config() noexcept override { return config_; }

  ErrorCode OnStart(StrategyContext& context) noexcept override {
    if (!config_.ready() || context.trading().accepting_orders()) {
      return ErrorCode::kInvalidState;
    }
    auto& market_data = context.market_data();
    const auto found = market_data.FindInstrument(
        config_.venue(), config_.symbol(), instrument_);
    if (found != ErrorCode::kOk) return found;
    account_available_ = context.trading().TryGetCash(initial_cash_);
    if (account_available_ &&
        (!context.trading().TryGetPosition(instrument_.id, initial_position_) ||
         initial_cash_.reserved != 0 ||
         initial_cash_.available != initial_cash_.balance ||
         initial_position_.reserved_qty != 0)) {
      return ErrorCode::kCorrupt;
    }
    const auto subscribed =
        market_data.SubscribeQuotes(config_.source_id(), instrument_.id);
    if (subscribed != ErrorCode::kOk) return subscribed;
    const auto printed = PrintMarker("order_lifecycle: ready\n");
    return printed == ErrorCode::kOk ? ReportAccount(context, "initial")
                                     : printed;
  }

  ErrorCode OnTick(StrategyContext& context,
                   const QuoteTick& tick) noexcept override {
    if (order_id_.value != 0 || !context.trading().accepting_orders() ||
        tick.instrument_id != instrument_.id ||
        !yquant::model::HasQuoteFlag(tick.flags, QuoteFlag::kAskValid) ||
        !yquant::model::HasQuoteFlag(tick.flags, QuoteFlag::kTradable)) {
      return ErrorCode::kOk;
    }
    OrderRequest request;
    request.instrument_id = instrument_.id;
    request.side = OrderSide::kBuy;
    request.limit_price = tick.ask_price;
    request.quantity = config_.quantity();
    const auto submitted = context.trading().SubmitOrder(request, order_id_);
    if (submitted != ErrorCode::kOk) return submitted;
    OrderSnapshot pending;
    if (!context.trading().TryGetOrder(order_id_, pending) ||
        pending.state != OrderState::kPendingNew ||
        pending.original_qty != config_.quantity() ||
        pending.cumulative_filled_qty != 0 ||
        pending.leaves_qty != config_.quantity() ||
        pending.report_sequence != 0 || pending.cancel_pending ||
        CheckAccount(context, pending) != ErrorCode::kOk) {
      return ErrorCode::kCorrupt;
    }
    return PrintMarker("order_lifecycle: submitted\n");
  }

  ErrorCode OnOrderUpdate(StrategyContext& context,
                          const OrderUpdate& update) noexcept override {
    OrderSnapshot queried;
    if (update.order.client_order_id != order_id_ ||
        !context.trading().TryGetOrder(order_id_, queried) ||
        queried != update.order || awaiting_fill_ ||
        CheckAccount(context, queried) != ErrorCode::kOk) {
      return ErrorCode::kCorrupt;
    }
    awaiting_fill_ = update.kind == OrderUpdateKind::kFill;
    if (update.kind == OrderUpdateKind::kAccepted) {
      return PrintMarker("order_lifecycle: working\n");
    }
    const auto state = queried.state;
    if (!terminal_reported_ &&
        (state == OrderState::kFilled || state == OrderState::kCanceled ||
         state == OrderState::kRejected)) {
      terminal_reported_ = true;
      const int written = std::fprintf(
          stderr,
          "order_lifecycle: terminal state=%s cumulative=%" PRId64
          " leaves=%" PRId64 "\n",
          StateName(state), queried.cumulative_filled_qty, queried.leaves_qty);
      return written < 0 || std::fflush(stderr) != 0 ? ErrorCode::kIoError
                                                     : ErrorCode::kOk;
    }
    return ErrorCode::kOk;
  }

  ErrorCode OnFill(StrategyContext& context,
                   const Fill& fill) noexcept override {
    OrderSnapshot queried;
    if (!awaiting_fill_ || fill.client_order_id != order_id_ ||
        fill.quantity <= 0 ||
        fill.quantity > config_.quantity() - filled_qty_ ||
        !context.trading().TryGetOrder(order_id_, queried) ||
        queried.cumulative_filled_qty != filled_qty_ + fill.quantity ||
        fill.instrument_id != instrument_.id || fill.side != OrderSide::kBuy ||
        fill.broker_order_id != queried.broker_order_id ||
        CheckAccount(context, queried) != ErrorCode::kOk) {
      return ErrorCode::kCorrupt;
    }
    const auto fill_cost = fill.price * PriceScale() * fill.quantity;
    CashSnapshot cash;
    if (!context.trading().TryGetCash(cash) ||
        cash.balance != initial_cash_.balance - filled_cost_ - fill_cost) {
      return ErrorCode::kCorrupt;
    }
    awaiting_fill_ = false;
    filled_qty_ += fill.quantity;
    filled_cost_ += fill_cost;
    ++fills_;
    return ReportAccount(context, "fill");
  }

  ErrorCode OnStop(StrategyContext& context) noexcept override {
    OrderSnapshot final;
    const bool has_order = order_id_.value != 0;
    if (context.trading().accepting_orders() ||
        (has_order && (!context.trading().TryGetOrder(order_id_, final) ||
                       CheckAccount(context, final) != ErrorCode::kOk))) {
      return ErrorCode::kCorrupt;
    }
    if (account_available_) {
      CashSnapshot cash;
      if (!context.trading().TryGetCash(cash) ||
          cash.balance != initial_cash_.balance - filled_cost_) {
        return ErrorCode::kCorrupt;
      }
    }
    const int written = std::fprintf(
        stderr,
        "order_lifecycle: stopped fills=%" PRIu64 " filled_qty=%" PRId64
        " state=%s\n",
        fills_, filled_qty_, has_order ? StateName(final.state) : "none");
    return written < 0 || std::fflush(stderr) != 0
               ? ErrorCode::kIoError
               : ReportAccount(context, "final");
  }

 private:
  std::int64_t PriceScale() const noexcept {
    std::int64_t scale = 1;
    for (std::uint8_t digits = instrument_.price_decimals; digits < 6;
         ++digits) {
      scale *= 10;
    }
    return scale;
  }

  // Account facts must be visible at entry to both trading callbacks. Buys
  // reserve the remaining limit cost; newly bought shares are not sellable.
  // Exact actual execution costs are checked when OnFill supplies its price.
  ErrorCode CheckAccount(const StrategyContext& context,
                         const OrderSnapshot& order) const noexcept {
    CashSnapshot cash;
    PositionSnapshot position;
    if (!account_available_ || !context.trading().TryGetCash(cash) ||
        !context.trading().TryGetPosition(instrument_.id, position)) {
      return ErrorCode::kCorrupt;
    }
    const auto unit_price = order.limit_price * PriceScale();
    if (cash.balance <
            initial_cash_.balance - unit_price * order.cumulative_filled_qty ||
        cash.balance > initial_cash_.balance ||
        cash.reserved != unit_price * order.leaves_qty ||
        cash.available != cash.balance - cash.reserved ||
        position.total_qty !=
            initial_position_.total_qty + order.cumulative_filled_qty ||
        position.available_qty != initial_position_.available_qty ||
        position.reserved_qty != 0) {
      return ErrorCode::kCorrupt;
    }
    return ErrorCode::kOk;
  }

  // Low-frequency account summaries use copied snapshots in micro-yuan/share
  // units. Disabled trading keeps the lifecycle example usable without orders.
  ErrorCode ReportAccount(const StrategyContext& context,
                          const char* stage) const noexcept {
    if (!account_available_) return ErrorCode::kOk;
    CashSnapshot cash;
    PositionSnapshot position;
    if (!context.trading().TryGetCash(cash) ||
        !context.trading().TryGetPosition(instrument_.id, position)) {
      return ErrorCode::kCorrupt;
    }
    const int written = std::fprintf(
        stderr,
        "order_lifecycle: account %s cash=%" PRId64 " reserved=%" PRId64
        " available=%" PRId64 " position_total=%" PRId64
        " position_available=%" PRId64 " position_reserved=%" PRId64 "\n",
        stage, cash.balance, cash.reserved, cash.available, position.total_qty,
        position.available_qty, position.reserved_qty);
    return written < 0 || std::fflush(stderr) != 0 ? ErrorCode::kIoError
                                                   : ErrorCode::kOk;
  }

  // Low-frequency handshake output; no per-quote formatting or borrowed text.
  static ErrorCode PrintMarker(const char* text) noexcept {
    return std::fputs(text, stderr) < 0 || std::fflush(stderr) != 0
               ? ErrorCode::kIoError
               : ErrorCode::kOk;
  }

  OrderLifecycleConfig config_;
  InstrumentDefinition instrument_;
  CashSnapshot initial_cash_;
  PositionSnapshot initial_position_;
  ClientOrderId order_id_;
  std::int64_t filled_qty_ = 0;
  std::int64_t filled_cost_ = 0;
  std::uint64_t fills_ = 0;
  bool awaiting_fill_ = false;
  bool terminal_reported_ = false;
  bool account_available_ = false;
};

// Failed allocation leaves output null; destroy also accepts an unstarted
// instance.
ErrorCode CreateStrategy(Strategy*& output) noexcept {
  output = new (std::nothrow) OrderLifecycleStrategy;
  return output == nullptr ? ErrorCode::kResourceExhausted : ErrorCode::kOk;
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
