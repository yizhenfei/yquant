#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string_view>

#include "simdjson.h"
#include "yquant/model/account.h"
#include "yquant/model/order.h"
#include "yquant/model/trading_event.h"
#include "yquant/strategy/strategy.h"
#include "yquant/strategy/strategy_plugin.h"

#ifndef YQUANT_TEST_TRADING_MODE
#define YQUANT_TEST_TRADING_MODE 0
#endif

namespace {

using yquant::base::ConfigError;
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
using yquant::model::QuoteTick;
using yquant::strategy::Strategy;
using yquant::strategy::StrategyConfig;
using yquant::strategy::StrategyContext;
using yquant::strategy::StrategyPlugin;

// Records bounded callback facts using the descriptor inherited by tests.
void Emit(char event) noexcept {
  const char* value = std::getenv("YQUANT_TEST_EVENT_FD");
  if (value == nullptr) return;
  const int fd = static_cast<int>(std::strtol(value, nullptr, 10));
  while (write(fd, &event, 1) == -1 && errno == EINTR) {
  }
}

class TradingFixtureConfig final : public StrategyConfig {
 public:
  std::int64_t quantity() const noexcept { return quantity_; }
  OrderSide side() const noexcept { return side_; }
  std::int64_t price_improvement() const noexcept { return price_improvement_; }

 protected:
  ErrorCode LoadParameters(std::string_view json,
                           ConfigError& error) noexcept override {
    Emit('P');
    simdjson::dom::parser parser;
    simdjson::dom::object object;
    if (parser.parse(json.data(), json.size()).get_object().get(object)) {
      error.Set("", "expected an object");
      return ErrorCode::kInvalidArgument;
    }
    std::int64_t quantity = 200;
    OrderSide side = OrderSide::kBuy;
    std::int64_t price_improvement = 0;
    for (const auto field : object) {
      if (field.key == "quantity") {
        if (field.value.get_int64().get(quantity) || quantity <= 0) {
          error.Set("quantity", "expected a positive integer");
          return ErrorCode::kInvalidArgument;
        }
      } else if (field.key == "side") {
        std::string_view value;
        if (field.value.get_string().get(value) ||
            (value != "buy" && value != "sell")) {
          error.Set("side", "expected buy or sell");
          return ErrorCode::kInvalidArgument;
        }
        side = value == "buy" ? OrderSide::kBuy : OrderSide::kSell;
      } else if (field.key == "price_improvement") {
        if (field.value.get_int64().get(price_improvement) ||
            price_improvement < 0 || price_improvement > 100) {
          error.Set("price_improvement", "expected an integer from 0 to 100");
          return ErrorCode::kInvalidArgument;
        }
      } else {
        error.Set(field.key, "unknown parameter");
        return ErrorCode::kInvalidArgument;
      }
    }
    side_ = side;
    price_improvement_ = price_improvement;
    quantity_ = quantity;
    return ErrorCode::kOk;
  }

 private:
  std::int64_t quantity_ = 200;
  OrderSide side_ = OrderSide::kBuy;
  std::int64_t price_improvement_ = 0;
};

// Exercises strategy-visible contracts only. Modes inject callback behavior;
// malformed broker reports come from host-side test adapters.
class TradingFixture final : public Strategy {
 public:
  TradingFixtureConfig& config() noexcept override { return config_; }

  ErrorCode OnStart(StrategyContext& context) noexcept override {
    auto& data = context.market_data();
    const auto found = data.FindInstrument("SSE", "600000", instrument_);
    if (found != ErrorCode::kOk) return found;
    if (context.trading().accepting_orders() ||
        !context.trading().TryGetCash(initial_cash_) ||
        !context.trading().TryGetPosition(instrument_.id, initial_position_) ||
        CheckAccount(context) != ErrorCode::kOk)
      return ErrorCode::kCorrupt;
    const auto request = Request(1001);
    ClientOrderId untouched{999};
    if (context.trading().SubmitOrder(request, untouched) !=
            ErrorCode::kInvalidState ||
        untouched.value != 999 ||
        context.trading().CancelOrder(untouched) != ErrorCode::kInvalidState) {
      return ErrorCode::kCorrupt;
    }
    const auto subscribed = data.SubscribeQuotes(instrument_.id);
    if (subscribed != ErrorCode::kOk) return subscribed;
    Emit('S');
    return ErrorCode::kOk;
  }

  ErrorCode OnTick(StrategyContext& context,
                   const QuoteTick& tick) noexcept override {
    Emit('Q');
    if (order_count_ != 0) return ErrorCode::kOk;
    if (!context.trading().accepting_orders()) return ErrorCode::kUnsupported;
    constexpr std::size_t kOrders = YQUANT_TEST_TRADING_MODE == 4 ? 200 : 1;
    const auto request = Request(
        config_.side() == OrderSide::kBuy ? tick.ask_price : tick.bid_price);
    if constexpr (YQUANT_TEST_TRADING_MODE == 8) {
      if (local_failure_checked_) return ErrorCode::kOk;
      local_failure_checked_ = true;
      ClientOrderId untouched{999};
      OrderSnapshot unknown;
      unknown.report_sequence = 999;
      const auto original = unknown;
      if (context.trading().SubmitOrder(request, untouched) !=
              ErrorCode::kResourceExhausted ||
          untouched.value != 999 ||
          context.trading().TryGetOrder({1}, unknown) || unknown != original ||
          CheckAccount(context) != ErrorCode::kOk) {
        return ErrorCode::kCorrupt;
      }
      Emit('L');
      return ErrorCode::kOk;
    }
    for (std::size_t index = 0; index < kOrders; ++index) {
      inside_submit_ = true;
      const auto result = context.trading().SubmitOrder(request, ids_[index]);
      inside_submit_ = false;
      if (result != ErrorCode::kOk) return result;
      ++order_count_;
      OrderSnapshot snapshot;
      if (!context.trading().TryGetOrder(ids_[index], snapshot) ||
          snapshot.client_order_id != ids_[index] ||
          snapshot.state != OrderState::kPendingNew ||
          snapshot.original_qty != config_.quantity() ||
          snapshot.cumulative_filled_qty != 0 ||
          snapshot.leaves_qty != config_.quantity() ||
          snapshot.report_sequence != 0) {
        return ErrorCode::kCorrupt;
      }
      if (CheckAccount(context) != ErrorCode::kOk) return ErrorCode::kCorrupt;
      Emit('N');
    }
    return ErrorCode::kOk;
  }

  ErrorCode OnOrderUpdate(StrategyContext& context,
                          const OrderUpdate& update) noexcept override {
    OrderSnapshot snapshot;
    if (CheckAccount(context) != ErrorCode::kOk || inside_submit_ ||
        awaiting_fill_ ||
        !context.trading().TryGetOrder(update.order.client_order_id,
                                       snapshot) ||
        snapshot != update.order)
      return ErrorCode::kCorrupt;
    awaiting_fill_ = update.kind == OrderUpdateKind::kFill;
    switch (update.kind) {
      case OrderUpdateKind::kAccepted:
        Emit('A');
        break;
      case OrderUpdateKind::kRejected:
        Emit('R');
        break;
      case OrderUpdateKind::kFill:
        Emit('V');
        break;
      case OrderUpdateKind::kCanceled:
        Emit('X');
        break;
      case OrderUpdateKind::kCancelRejected:
        Emit('J');
        break;
      case OrderUpdateKind::kCanceledBeforeSend:
        Emit('B');
        break;
    }
    if constexpr (YQUANT_TEST_TRADING_MODE == 2) {
      return ErrorCode::kInvalidArgument;
    }
    if constexpr (YQUANT_TEST_TRADING_MODE == 1 ||
                  YQUANT_TEST_TRADING_MODE == 6) {
      const bool cancel_now = YQUANT_TEST_TRADING_MODE == 1
                                  ? update.kind == OrderUpdateKind::kAccepted
                                  : update.kind == OrderUpdateKind::kFill &&
                                        snapshot.state == OrderState::kWorking;
      if (cancel_now && context.trading().accepting_orders()) {
        const auto canceled =
            context.trading().CancelOrder(snapshot.client_order_id);
        if (canceled != ErrorCode::kOk) return canceled;
        OrderSnapshot pending;
        if (!context.trading().TryGetOrder(snapshot.client_order_id, pending) ||
            !pending.cancel_pending ||
            pending.leaves_qty != snapshot.leaves_qty ||
            pending.cumulative_filled_qty != snapshot.cumulative_filled_qty) {
          return ErrorCode::kCorrupt;
        }
        Emit('K');
      }
    }
    return ErrorCode::kOk;
  }

  ErrorCode OnFill(StrategyContext& context,
                   const Fill& fill) noexcept override {
    OrderSnapshot snapshot;
    if (CheckAccount(context) != ErrorCode::kOk || inside_submit_ ||
        !awaiting_fill_ || fill.quantity <= 0 ||
        !context.trading().TryGetOrder(fill.client_order_id, snapshot) ||
        snapshot.cumulative_filled_qty != filled_qty_ + fill.quantity ||
        (fill.side == OrderSide::kBuy ? fill.price > snapshot.limit_price
                                      : fill.price < snapshot.limit_price) ||
        fill.instrument_id != instrument_.id || fill.side != config_.side() ||
        fill.price !=
            (fill.side == OrderSide::kBuy
                 ? snapshot.limit_price - config_.price_improvement()
                 : snapshot.limit_price + config_.price_improvement()))
      return ErrorCode::kCorrupt;
    awaiting_fill_ = false;
    filled_qty_ += fill.quantity;
    Emit('F');
    if (fill.quantity == 100) Emit('H');
    if (fill.quantity == 1) Emit('I');
    if constexpr (YQUANT_TEST_TRADING_MODE == 3)
      return ErrorCode::kInvalidArgument;
    return ErrorCode::kOk;
  }

  ErrorCode OnStop(StrategyContext& context) noexcept override {
    if (context.trading().accepting_orders() ||
        CheckAccount(context) != ErrorCode::kOk) {
      return ErrorCode::kCorrupt;
    }
    for (std::size_t index = 0; index < order_count_; ++index) {
      OrderSnapshot snapshot;
      if (!context.trading().TryGetOrder(ids_[index], snapshot))
        return ErrorCode::kCorrupt;
      ClientOrderId unchanged{999};
      if (context.trading().SubmitOrder(Request(1001), unchanged) !=
              ErrorCode::kInvalidState ||
          unchanged.value != 999 ||
          context.trading().CancelOrder(ids_[index]) !=
              ErrorCode::kInvalidState) {
        return ErrorCode::kCorrupt;
      }
      if constexpr (YQUANT_TEST_TRADING_MODE == 0 ||
                    YQUANT_TEST_TRADING_MODE == 1 ||
                    YQUANT_TEST_TRADING_MODE == 4 ||
                    YQUANT_TEST_TRADING_MODE == 6) {
        if (order_count_ == 1 &&
            filled_qty_ != snapshot.cumulative_filled_qty) {
          return ErrorCode::kCorrupt;
        }
      }
      Emit(snapshot.state == OrderState::kFilled     ? 'f'
           : snapshot.state == OrderState::kCanceled ? 'c'
           : snapshot.state == OrderState::kRejected ? 'r'
           : snapshot.state == OrderState::kWorking  ? 'w'
                                                     : 'p');
    }
    Emit('T');
    return ErrorCode::kOk;
  }

 private:
  // The gateway account can include other clients and can advance before this
  // client's order status. Validate account invariants independently of orders.
  ErrorCode CheckAccount(const StrategyContext& context) const noexcept {
    CashSnapshot cash;
    PositionSnapshot position;
    if (!context.trading().TryGetCash(cash) ||
        !context.trading().TryGetPosition(instrument_.id, position) ||
        cash.balance < 0 || cash.reserved < 0 || cash.reserved > cash.balance ||
        cash.available != cash.balance - cash.reserved ||
        position.total_qty < 0 || position.available_qty < 0 ||
        position.available_qty > position.total_qty ||
        position.reserved_qty < 0 ||
        position.reserved_qty > position.total_qty - position.available_qty)
      return ErrorCode::kCorrupt;
    return ErrorCode::kOk;
  }

  OrderRequest Request(std::int64_t price) const noexcept {
    OrderRequest request;
    request.instrument_id = instrument_.id;
    request.side = config_.side();
    request.limit_price = price;
    request.quantity = config_.quantity();
    return request;
  }
  TradingFixtureConfig config_;
  InstrumentDefinition instrument_;
  CashSnapshot initial_cash_;
  PositionSnapshot initial_position_;
  std::array<ClientOrderId, 200> ids_ = {};
  std::size_t order_count_ = 0;
  std::int64_t filled_qty_ = 0;
  bool local_failure_checked_ = false;
  bool inside_submit_ = false;
  bool awaiting_fill_ = false;
};

ErrorCode Create(Strategy*& out) noexcept {
  Emit('C');
  out = new (std::nothrow) TradingFixture;
  return out == nullptr ? ErrorCode::kResourceExhausted : ErrorCode::kOk;
}
void Destroy(Strategy* strategy) noexcept {
  delete strategy;
  Emit('D');
}
const StrategyPlugin kPlugin = {
    yquant::strategy::kStrategyPluginAbiVersion,
    static_cast<std::uint32_t>(sizeof(StrategyPlugin)), Create, Destroy};

}  // namespace

extern "C" YQUANT_STRATEGY_EXPORT const StrategyPlugin*
YQuantGetStrategyPlugin() noexcept {
  return &kPlugin;
}
