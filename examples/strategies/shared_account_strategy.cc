#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string_view>

#include "simdjson.h"
#include "yquant/model/identifiers.h"
#include "yquant/model/instrument.h"
#include "yquant/strategy/strategy.h"
#include "yquant/strategy/strategy_plugin.h"

namespace {
using yquant::base::ErrorCode;
namespace model = yquant::model;
namespace strategy = yquant::strategy;

// Owns a single order's selection. Shared account state is queried from the
// trading service and never interpreted as this strategy's private allocation.
class Config final : public strategy::StrategyConfig {
 public:
  model::InstrumentId instrument_id;
  model::OrderSide side = model::OrderSide::kUnknown;
  std::int64_t quantity = 0;
  char venue_name[5] = {};
  char symbol_name[model::kSymbolLength + 1] = {};

 protected:
  ErrorCode LoadParameters(std::string_view json,
                           strategy::ConfigError& error) noexcept override {
    simdjson::dom::parser parser;
    simdjson::dom::object object;
    const auto code =
        parser.parse(json.data(), json.size()).get_object().get(object);
    if (code) {
      error.Set("", simdjson::error_message(code));
      return code == simdjson::MEMALLOC ? ErrorCode::kResourceExhausted
                                        : ErrorCode::kInvalidArgument;
    }
    std::string_view venue;
    std::string_view symbol;
    for (const auto field : object) {
      bool invalid = false;
      if (field.key == "venue") {
        invalid = field.value.get_string().get(venue);
      } else if (field.key == "symbol") {
        invalid = field.value.get_string().get(symbol);
      } else if (field.key == "side") {
        std::string_view value;
        invalid = field.value.get_string().get(value);
        if (value == "buy")
          side = model::OrderSide::kBuy;
        else if (value == "sell")
          side = model::OrderSide::kSell;
        else
          invalid = true;
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
    if (quantity <= 0 || side == model::OrderSide::kUnknown ||
        model::MakeInstrumentId(venue, symbol, instrument_id) !=
            ErrorCode::kOk) {
      error.Set("", "venue, symbol, side and quantity are required");
      return ErrorCode::kInvalidArgument;
    }
    std::memcpy(venue_name, venue.data(), venue.size());
    std::memcpy(symbol_name, symbol.data(), symbol.size());
    return ErrorCode::kOk;
  }
};

const char* StateName(model::OrderState state) noexcept {
  switch (state) {
    case model::OrderState::kPendingNew:
      return "pending";
    case model::OrderState::kWorking:
      return "working";
    case model::OrderState::kFilled:
      return "filled";
    case model::OrderState::kCanceled:
      return "canceled";
    case model::OrderState::kRejected:
      return "rejected";
    default:
      return "unknown";
  }
}

// Submits one order on the first usable quote. Order callbacks describe this
// strategy's order; account queries describe the shared account as a whole.
class SharedAccountStrategy final : public strategy::Strategy {
 public:
  strategy::StrategyConfig& config() noexcept override { return config_; }
  ErrorCode OnStart(strategy::StrategyContext& context) noexcept override {
    model::InstrumentDefinition definition;
    model::CashSnapshot cash;
    if (context.market_data().FindInstrument(config_.venue_name,
                                             config_.symbol_name,
                                             definition) != ErrorCode::kOk ||
        !context.trading().TryGetCash(cash))
      return ErrorCode::kInvalidArgument;
    const auto status =
        context.market_data().SubscribeQuotes(config_.instrument_id);
    if (status != ErrorCode::kOk) return status;
    return Marker("shared_account: ready\n");
  }
  ErrorCode OnTick(strategy::StrategyContext& context,
                   const model::QuoteTick& quote) noexcept override {
    if (order_id_.value != 0 || !context.trading().accepting_orders())
      return ErrorCode::kOk;
    const bool buy = config_.side == model::OrderSide::kBuy;
    const auto flag =
        buy ? model::QuoteFlag::kAskValid : model::QuoteFlag::kBidValid;
    if (!model::HasQuoteFlag(quote.flags, flag) ||
        !model::HasQuoteFlag(quote.flags, model::QuoteFlag::kTradable) ||
        (buy ? quote.ask_price : quote.bid_price) <= 0)
      return ErrorCode::kOk;
    const model::OrderRequest request{config_.instrument_id, config_.side,
                                      buy ? quote.ask_price : quote.bid_price,
                                      config_.quantity};
    const auto status = context.trading().SubmitOrder(request, order_id_);
    if (status != ErrorCode::kOk) return status;
    return Marker("shared_account: submitted\n");
  }
  ErrorCode OnOrderUpdate(strategy::StrategyContext& context,
                          const model::OrderUpdate& update) noexcept override {
    model::OrderSnapshot order;
    if (update.order.strategy_order_id != order_id_ ||
        !context.trading().TryGetOrder(order_id_, order) ||
        order != update.order)
      return ErrorCode::kCorrupt;
    model::CashSnapshot cash;
    if (!context.trading().TryGetCash(cash)) return ErrorCode::kCorrupt;
    const int written = std::fprintf(
        stdout,
        "shared_account: order state=%s cumulative=%" PRId64 " cash=%" PRId64
        " reserved=%" PRId64 " available=%" PRId64 "\n",
        StateName(order.state), order.cumulative_filled_qty, cash.balance,
        cash.reserved, cash.available);
    return written < 0 || std::fflush(stdout) != 0 ? ErrorCode::kIoError
                                                   : ErrorCode::kOk;
  }
  ErrorCode OnFill(strategy::StrategyContext&,
                   const model::Fill& fill) noexcept override {
    if (fill.strategy_order_id != order_id_ || fill.quantity <= 0 ||
        filled_qty_ > config_.quantity - fill.quantity)
      return ErrorCode::kCorrupt;
    filled_qty_ += fill.quantity;
    return ErrorCode::kOk;
  }
  ErrorCode OnStop(strategy::StrategyContext& context) noexcept override {
    model::OrderSnapshot order;
    model::CashSnapshot cash;
    if (!context.trading().TryGetCash(cash)) return ErrorCode::kCorrupt;
    const bool known = context.trading().TryGetOrder(order_id_, order);
    const int written = std::fprintf(
        stdout,
        "shared_account: stopped state=%s filled_qty=%" PRId64 " cash=%" PRId64
        " reserved=%" PRId64 " available=%" PRId64 "\n",
        known ? StateName(order.state) : "none", filled_qty_, cash.balance,
        cash.reserved, cash.available);
    return written < 0 || std::fflush(stdout) != 0 ? ErrorCode::kIoError
                                                   : ErrorCode::kOk;
  }

 private:
  static ErrorCode Marker(const char* text) noexcept {
    return std::fputs(text, stdout) < 0 || std::fflush(stdout) != 0
               ? ErrorCode::kIoError
               : ErrorCode::kOk;
  }
  Config config_;
  model::StrategyOrderId order_id_;
  std::int64_t filled_qty_ = 0;
};

ErrorCode Create(strategy::Strategy*& output) noexcept {
  output = new (std::nothrow) SharedAccountStrategy;
  return output ? ErrorCode::kOk : ErrorCode::kResourceExhausted;
}
void Destroy(strategy::Strategy* instance) noexcept { delete instance; }
const strategy::StrategyPlugin kPlugin = {
    strategy::kStrategyPluginAbiVersion,
    static_cast<std::uint32_t>(sizeof(strategy::StrategyPlugin)), Create,
    Destroy};
}  // namespace

extern "C" YQUANT_STRATEGY_EXPORT const yquant::strategy::StrategyPlugin*
YQuantGetStrategyPlugin() noexcept {
  return &kPlugin;
}
