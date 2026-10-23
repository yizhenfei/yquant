#include <stdint.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>

#include "simdjson.h"
#include "yquant/model/identifiers.h"
#include "yquant/model/instrument.h"
#include "yquant/model/quote_tick.h"
#include "yquant/strategy/strategy.h"
#include "yquant/strategy/strategy_context.h"
#include "yquant/strategy/strategy_plugin.h"

// Each target selects a distinct plugin failure, exercising the real loader.
#ifndef YQUANT_TEST_PLUGIN_MODE
#define YQUANT_TEST_PLUGIN_MODE 0
#endif

extern "C" int YQuantTestDependency() noexcept;

namespace {

using yquant::base::ConfigError;
using yquant::base::ErrorCode;
using yquant::strategy::Strategy;
using yquant::strategy::StrategyConfig;
using yquant::strategy::StrategyContext;
using yquant::strategy::StrategyPlugin;

void Emit(char event) noexcept {
  const char* value = std::getenv("YQUANT_TEST_EVENT_FD");
  if (value == nullptr) {
    return;
  }
  const int fd = static_cast<int>(std::strtol(value, nullptr, 10));
  while (write(fd, &event, 1) == -1 && errno == EINTR) {
  }
}

struct ModuleLifetime {
  ~ModuleLifetime() { Emit('U'); }
};

ModuleLifetime module_lifetime;

class FixtureConfig final : public StrategyConfig {
 public:
  std::string_view venue() const noexcept { return venue_; }
  std::string_view symbol() const noexcept { return symbol_; }

  bool values_valid() const noexcept {
    return quote_qty_ > 0 && quote_qty_ <= 100 &&
           large_value_ == 9007199254740993LL &&
           std::strcmp(label_, "owned label") == 0;
  }

 protected:
  ErrorCode LoadParameters(std::string_view json,
                           ConfigError& error) noexcept override {
    Emit('P');
    if constexpr (YQUANT_TEST_PLUGIN_MODE >= 14) {
      return LoadQuoteParameters(json, error);
    }
    if constexpr (YQUANT_TEST_PLUGIN_MODE != 13) {
      return StrategyConfig::LoadParameters(json, error);
    }
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    simdjson::dom::object object;
    if (parser.parse(json.data(), json.size()).get(root) ||
        root.get_object().get(object)) {
      error.Set("", "expected an object");
      return ErrorCode::kInvalidArgument;
    }
    bool has_qty = false;
    bool has_label = false;
    bool has_large = false;
    int64_t quote_qty = 0;
    int64_t large_value = 0;
    std::string_view label;
    for (const auto field : object) {
      if (field.key == "quote_qty") {
        if (field.value.get_int64().get(quote_qty) || quote_qty <= 0 ||
            quote_qty > 100) {
          error.Set("quote_qty", "expected an integer from 1 to 100");
          return ErrorCode::kInvalidArgument;
        }
        has_qty = true;
      } else if (field.key == "large_value") {
        if (field.value.get_int64().get(large_value) ||
            large_value != 9007199254740993LL) {
          error.Set("large_value", "expected exact integer 9007199254740993");
          return ErrorCode::kInvalidArgument;
        }
        has_large = true;
      } else if (field.key == "label") {
        if (field.value.get_string().get(label) || label.empty() ||
            label.size() >= sizeof(label_) ||
            label.find('\0') != std::string_view::npos) {
          error.Set("label", "expected a nonempty string of at most 63 bytes");
          return ErrorCode::kInvalidArgument;
        }
        has_label = true;
      } else {
        error.Set(field.key, "unknown parameter");
        return ErrorCode::kInvalidArgument;
      }
    }
    if (!has_qty || !has_label || !has_large) {
      error.Set(!has_qty     ? "quote_qty"
                : !has_label ? "label"
                             : "large_value",
                "required parameter missing");
      return ErrorCode::kInvalidArgument;
    }
    quote_qty_ = quote_qty;
    large_value_ = large_value;
    std::memcpy(label_, label.data(), label.size());
    label_[label.size()] = '\0';
    return ErrorCode::kOk;
  }

 private:
  ErrorCode LoadQuoteParameters(std::string_view json,
                                ConfigError& error) noexcept {
    simdjson::dom::parser parser;
    simdjson::dom::object object;
    if (parser.parse(json.data(), json.size()).get_object().get(object)) {
      error.Set("", "expected an object");
      return ErrorCode::kInvalidArgument;
    }
    std::string_view venue;
    std::string_view symbol;
    for (const auto field : object) {
      if (field.key == "venue") {
        if (field.value.get_string().get(venue) ||
            (venue != "SSE" && venue != "SZSE" && venue != "BSE")) {
          error.Set("venue", "expected a supported venue");
          return ErrorCode::kInvalidArgument;
        }
      } else if (field.key == "symbol") {
        if (field.value.get_string().get(symbol) ||
            symbol.size() != yquant::model::kSymbolLength ||
            symbol.find_first_not_of("0123456789") != std::string_view::npos) {
          error.Set("symbol", "expected a six digit symbol");
          return ErrorCode::kInvalidArgument;
        }
      } else {
        error.Set(field.key, "unknown parameter");
        return ErrorCode::kInvalidArgument;
      }
    }
    if (venue.empty() || symbol.empty()) {
      error.Set("", "venue and symbol are required");
      return ErrorCode::kInvalidArgument;
    }
    std::memcpy(venue_, venue.data(), venue.size());
    std::memcpy(symbol_, symbol.data(), symbol.size());
    return ErrorCode::kOk;
  }

  int64_t quote_qty_ = 0;
  int64_t large_value_ = 0;
  char label_[64] = {};
  char venue_[5] = {};
  char symbol_[yquant::model::kSymbolLength + 1] = {};
};

class FixtureStrategy final : public Strategy {
 public:
  FixtureConfig& config() noexcept override { return config_; }

  ErrorCode OnStart(StrategyContext& context) noexcept override {
    if (!config_.ready() || config_.strategy_id().empty()) {
      return ErrorCode::kInvalidState;
    }
    if constexpr (YQUANT_TEST_PLUGIN_MODE == 13) {
      if (!config_.values_valid()) {
        return ErrorCode::kInvalidState;
      }
    }
    if constexpr (YQUANT_TEST_PLUGIN_MODE >= 14) {
      const ErrorCode result = StartQuotes(context);
      if (result != ErrorCode::kOk) {
        return result;
      }
    }
    Emit('S');
    if constexpr (YQUANT_TEST_PLUGIN_MODE == 12) {
      if (YQuantTestDependency() != 1) {
        return ErrorCode::kInternalError;
      }
    }
    const char* value = std::getenv("YQUANT_TEST_START_RELEASE_FD");
    if (value != nullptr) {
      const int fd = static_cast<int>(std::strtol(value, nullptr, 10));
      char release = 0;
      while (read(fd, &release, 1) == -1 && errno == EINTR) {
      }
    }
    if constexpr (YQUANT_TEST_PLUGIN_MODE == 9 ||
                  YQUANT_TEST_PLUGIN_MODE == 16) {
      return ErrorCode::kInvalidArgument;
    }
    return ErrorCode::kOk;
  }

  ErrorCode OnStop(StrategyContext& context) noexcept override {
    if constexpr (YQUANT_TEST_PLUGIN_MODE >= 14) {
      auto& market_data = context.market_data();
      yquant::model::QuoteTick snapshot;
      if (last_sequence_ != 0 &&
          (!market_data.TryGetLatestQuote(instrument_id_, snapshot) ||
           snapshot.sequence != last_sequence_)) {
        return ErrorCode::kCorrupt;
      }
      const auto expected = snapshot;
      if (market_data.TryGetLatestQuote({}, snapshot) ||
          !SameQuote(snapshot, expected)) {
        return ErrorCode::kCorrupt;
      }
      if (market_data.SubscribeQuotes(instrument_id_) !=
          ErrorCode::kInvalidState) {
        return ErrorCode::kCorrupt;
      }
    }
    Emit('T');
    if constexpr (YQUANT_TEST_PLUGIN_MODE == 10) {
      return ErrorCode::kInvalidArgument;
    }
    return ErrorCode::kOk;
  }

  ErrorCode OnTick(StrategyContext& context,
                   const yquant::model::QuoteTick& tick) noexcept override {
    if constexpr (YQUANT_TEST_PLUGIN_MODE >= 14) {
      auto& market_data = context.market_data();
      yquant::model::QuoteTick snapshot;
      if (tick.sequence <= last_sequence_ ||
          !market_data.TryGetLatestQuote(tick.instrument_id, snapshot) ||
          !SameQuote(snapshot, tick) || context.Now() < tick.receive_time_ns ||
          market_data.SubscribeQuotes(tick.instrument_id) !=
              ErrorCode::kInvalidState) {
        return ErrorCode::kCorrupt;
      }
      const auto expected = snapshot;
      if (market_data.TryGetLatestQuote({}, snapshot) ||
          !SameQuote(snapshot, expected)) {
        return ErrorCode::kCorrupt;
      }
      last_sequence_ = tick.sequence;
      Emit('Q');
      if constexpr (YQUANT_TEST_PLUGIN_MODE == 15) {
        return ErrorCode::kInvalidArgument;
      }
    }
    return ErrorCode::kOk;
  }

 private:
  static bool SameQuote(const yquant::model::QuoteTick& left,
                        const yquant::model::QuoteTick& right) noexcept {
    return left.source_id == right.source_id &&
           left.instrument_id == right.instrument_id &&
           left.sequence == right.sequence &&
           left.event_time_ns == right.event_time_ns &&
           left.receive_time_ns == right.receive_time_ns &&
           left.bid_price == right.bid_price && left.bid_qty == right.bid_qty &&
           left.ask_price == right.ask_price && left.ask_qty == right.ask_qty &&
           left.flags == right.flags;
  }

  ErrorCode StartQuotes(StrategyContext& context) noexcept {
    auto& market_data = context.market_data();
    yquant::model::InstrumentDefinition instrument;
    const ErrorCode lookup = market_data.FindInstrument(
        config_.venue(), config_.symbol(), instrument);
    if (lookup != ErrorCode::kOk) {
      return lookup;
    }
    instrument_id_ = instrument.id;
    auto unchanged = instrument;
    if (market_data.FindInstrument("INVALID", config_.symbol(), unchanged) !=
            ErrorCode::kInvalidArgument ||
        unchanged != instrument ||
        market_data.SubscribeQuotes({}) != ErrorCode::kInvalidArgument) {
      return ErrorCode::kCorrupt;
    }
    const ErrorCode subscribe = market_data.SubscribeQuotes(instrument_id_);
    if (subscribe != ErrorCode::kOk ||
        market_data.SubscribeQuotes(instrument_id_) != ErrorCode::kOk) {
      return subscribe == ErrorCode::kOk ? ErrorCode::kCorrupt : subscribe;
    }
    yquant::model::QuoteTick untouched;
    untouched.sequence = 999;
    if (market_data.TryGetLatestQuote(instrument_id_, untouched) ||
        untouched.sequence != 999) {
      return ErrorCode::kCorrupt;
    }
    return ErrorCode::kOk;
  }

  FixtureConfig config_;
  yquant::model::InstrumentId instrument_id_;
  uint64_t last_sequence_ = 0;
};

ErrorCode Create(Strategy*& strategy) noexcept {
  Emit('C');
  if constexpr (YQUANT_TEST_PLUGIN_MODE == 6) {
    strategy = nullptr;
    return ErrorCode::kInvalidArgument;
  }
  if constexpr (YQUANT_TEST_PLUGIN_MODE == 7) {
    strategy = nullptr;
    return ErrorCode::kOk;
  }
  strategy = new (std::nothrow) FixtureStrategy;
  if constexpr (YQUANT_TEST_PLUGIN_MODE == 8) {
    return ErrorCode::kInvalidArgument;
  }
  return strategy != nullptr ? ErrorCode::kOk : ErrorCode::kResourceExhausted;
}

void Destroy(Strategy* strategy) noexcept {
  delete strategy;
  Emit('D');
}

[[maybe_unused]] const StrategyPlugin plugin = {
    YQUANT_TEST_PLUGIN_MODE == 2
        ? yquant::strategy::kStrategyPluginAbiVersion - 1
        : yquant::strategy::kStrategyPluginAbiVersion,
    YQUANT_TEST_PLUGIN_MODE == 3
        ? 0U
        : static_cast<uint32_t>(sizeof(StrategyPlugin)),
    YQUANT_TEST_PLUGIN_MODE == 4 ? nullptr : &Create,
    YQUANT_TEST_PLUGIN_MODE == 5 ? nullptr : &Destroy,
};

}  // namespace

#if YQUANT_TEST_PLUGIN_MODE != 1
extern "C" YQUANT_STRATEGY_EXPORT const StrategyPlugin*
YQuantGetStrategyPlugin() noexcept {
  if constexpr (YQUANT_TEST_PLUGIN_MODE == 11) {
    return nullptr;
  }
  return &plugin;
}
#endif
