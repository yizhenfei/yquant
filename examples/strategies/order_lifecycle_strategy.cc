#include <stddef.h>
#include <stdint.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>
#include <string_view>

#include "simdjson.h"
#include "yquant/base/format.h"
#include "yquant/model/account.h"
#include "yquant/model/identifiers.h"
#include "yquant/model/order.h"
#include "yquant/model/trading_event.h"
#include "yquant/strategy/strategy.h"
#include "yquant/strategy/strategy_plugin.h"

namespace {

using yquant::base::ErrorCode;
using yquant::model::CashSnapshot;
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
using yquant::model::StrategyOrderId;
using yquant::strategy::ConfigError;
using yquant::strategy::Strategy;
using yquant::strategy::StrategyConfig;
using yquant::strategy::StrategyContext;
using yquant::strategy::StrategyPlugin;

// Owns the instrument selection and quantity; the parser/input die at startup.
class OrderLifecycleConfig final : public StrategyConfig {
 public:
  std::string_view venue() const noexcept { return {venue_, venue_size_}; }
  std::string_view symbol() const noexcept {
    return {symbol_, sizeof(symbol_)};
  }
  int64_t quantity() const noexcept { return quantity_; }

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
    std::string_view venue;
    std::string_view symbol;
    int64_t quantity = 0;
    for (const auto field : object) {
      bool invalid = false;
      if (field.key == "venue") {
        invalid = field.value.get_string().get(venue) ||
                  (venue != "SSE" && venue != "SZSE" && venue != "BSE");
      } else if (field.key == "symbol") {
        invalid =
            field.value.get_string().get(symbol) ||
            symbol.size() != yquant::model::kSymbolLength ||
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
    if (venue.empty() || symbol.empty() || quantity == 0) {
      error.Set(venue.empty()    ? "venue"
                : symbol.empty() ? "symbol"
                                 : "quantity",
                "required parameter is missing");
      return ErrorCode::kInvalidArgument;
    }
    std::memcpy(venue_, venue.data(), venue.size());
    venue_size_ = venue.size();
    std::memcpy(symbol_, symbol.data(), symbol.size());
    quantity_ = quantity;
    return ErrorCode::kOk;
  }

 private:
  char venue_[4] = {};
  size_t venue_size_ = 0;
  char symbol_[yquant::model::kSymbolLength] = {};
  int64_t quantity_ = 0;
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
    if (found != ErrorCode::kOk) {
      return found;
    }
    if (CheckAccount(context, {}) != ErrorCode::kOk) {
      return ErrorCode::kCorrupt;
    }
    const auto subscribed = market_data.SubscribeQuotes(instrument_.id);
    if (subscribed != ErrorCode::kOk) {
      return subscribed;
    }
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
    if (submitted != ErrorCode::kOk) {
      return submitted;
    }
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
    if (update.order.strategy_order_id != order_id_ ||
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
      const int written = ::yquant::base::Print(
          stdout,
          "order_lifecycle: terminal state={} cumulative={} leaves={}\n",
          StateName(state), queried.cumulative_filled_qty, queried.leaves_qty);
      return written < 0 || std::fflush(stdout) != 0 ? ErrorCode::kIoError
                                                     : ErrorCode::kOk;
    }
    return ErrorCode::kOk;
  }

  ErrorCode OnFill(StrategyContext& context,
                   const Fill& fill) noexcept override {
    OrderSnapshot queried;
    if (!awaiting_fill_ || fill.strategy_order_id != order_id_ ||
        fill.quantity <= 0 ||
        fill.quantity > config_.quantity() - filled_qty_ ||
        !context.trading().TryGetOrder(order_id_, queried) ||
        queried.cumulative_filled_qty != filled_qty_ + fill.quantity ||
        fill.instrument_id != instrument_.id || fill.side != OrderSide::kBuy ||
        fill.adapter_order_id != queried.adapter_order_id ||
        CheckAccount(context, queried) != ErrorCode::kOk) {
      return ErrorCode::kCorrupt;
    }
    awaiting_fill_ = false;
    filled_qty_ += fill.quantity;
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
    const int written = ::yquant::base::Print(
        stdout, "order_lifecycle: stopped fills={} filled_qty={} state={}\n",
        fills_, filled_qty_, has_order ? StateName(final.state) : "none");
    return written < 0 || std::fflush(stdout) != 0
               ? ErrorCode::kIoError
               : ReportAccount(context, "final");
  }

 private:
  // Gateway snapshots may include other strategies and advance before this
  // client's order status. Validate their own invariants independently.
  ErrorCode CheckAccount(const StrategyContext& context,
                         const OrderSnapshot&) const noexcept {
    CashSnapshot cash;
    PositionSnapshot position;
    if (!context.trading().TryGetCash(cash) ||
        !context.trading().TryGetPosition(instrument_.id, position) ||
        cash.balance < 0 || cash.reserved < 0 || cash.reserved > cash.balance ||
        cash.available != cash.balance - cash.reserved ||
        position.total_qty < 0 || position.available_qty < 0 ||
        position.available_qty > position.total_qty ||
        position.reserved_qty < 0 ||
        position.reserved_qty > position.total_qty - position.available_qty) {
      return ErrorCode::kCorrupt;
    }
    return ErrorCode::kOk;
  }

  // Low-frequency account summaries use copied snapshots in micro-yuan/share
  // units. The gateway publishes these snapshots with execution updates.
  ErrorCode ReportAccount(const StrategyContext& context,
                          const char* stage) const noexcept {
    CashSnapshot cash;
    PositionSnapshot position;
    if (!context.trading().TryGetCash(cash) ||
        !context.trading().TryGetPosition(instrument_.id, position)) {
      return ErrorCode::kCorrupt;
    }
    const int written = ::yquant::base::Print(
        stdout,
        "order_lifecycle: account {} cash={} reserved={} available={} "
        "position_total={} position_available={} position_reserved={}\n",
        stage, cash.balance, cash.reserved, cash.available, position.total_qty,
        position.available_qty, position.reserved_qty);
    return written < 0 || std::fflush(stdout) != 0 ? ErrorCode::kIoError
                                                   : ErrorCode::kOk;
  }

  // Low-frequency handshake output; no per-quote formatting or borrowed text.
  static ErrorCode PrintMarker(const char* text) noexcept {
    return std::fputs(text, stdout) < 0 || std::fflush(stdout) != 0
               ? ErrorCode::kIoError
               : ErrorCode::kOk;
  }

  OrderLifecycleConfig config_;
  InstrumentDefinition instrument_;
  StrategyOrderId order_id_;
  int64_t filled_qty_ = 0;
  uint64_t fills_ = 0;
  bool awaiting_fill_ = false;
  bool terminal_reported_ = false;
};

// Failed allocation leaves output null; destroy also accepts an unstarted
// instance.
ErrorCode CreateStrategy(Strategy*& output) noexcept {
  output = new (std::nothrow) OrderLifecycleStrategy;
  return output == nullptr ? ErrorCode::kResourceExhausted : ErrorCode::kOk;
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
