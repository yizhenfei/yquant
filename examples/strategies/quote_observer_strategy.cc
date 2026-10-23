// Example strategy plugin for observing a configured instrument's quotes.
//
// The configuration stores a validated venue/symbol pair. OnStart resolves the
// instrument and subscribes to quotes from the host's bound source. OnTick
// checks the instrument, verifies the host's latest snapshot matches the
// callback and sequence numbers increase, then counts quotes and tracks the
// maximum local age using Now() - receive_time_ns.
// The first quote emits a readiness marker; OnStop prints the final summary.
// YQuantGetStrategyPlugin exports the module's creation and destruction hooks.

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>

#include "simdjson.h"
#include "yquant/base/format.h"
#include "yquant/model/identifiers.h"
#include "yquant/model/instrument.h"
#include "yquant/model/quote_tick.h"
#include "yquant/strategy/strategy.h"
#include "yquant/strategy/strategy_plugin.h"

namespace {

using yquant::base::ErrorCode;
using yquant::model::InstrumentDefinition;
using yquant::model::InstrumentId;
using yquant::model::QuoteTick;
using yquant::strategy::ConfigError;
using yquant::strategy::Strategy;
using yquant::strategy::StrategyConfig;
using yquant::strategy::StrategyContext;
using yquant::strategy::StrategyPlugin;

// Copies parser diagnostics before the parser and its input are destroyed.
ErrorCode ParseError(simdjson::error_code result, std::string_view path,
                     ConfigError& error) noexcept {
  error.Set(path, simdjson::error_message(result));
  return result == simdjson::MEMALLOC ? ErrorCode::kResourceExhausted
                                      : ErrorCode::kInvalidArgument;
}

// Writes the first-quote marker without stdio buffering or allocation.
// Retries interrupted writes; other failures return kIoError after any prefix
// already written. The text is borrowed only until this function returns.
ErrorCode WriteReadiness(std::string_view text) noexcept {
  while (!text.empty()) {
    const ssize_t written = write(STDOUT_FILENO, text.data(), text.size());
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

// Owns a validated venue/symbol pair. Initialization commits all values
// together, retaining no JSON strings or parser objects.
class QuoteObserverConfig final : public StrategyConfig {
 public:
  std::string_view venue() const noexcept { return {venue_, venue_size_}; }
  std::string_view symbol() const noexcept {
    return {symbol_, sizeof(symbol_)};
  }

 protected:
  ErrorCode LoadParameters(std::string_view parameters_json,
                           ConfigError& error) noexcept override {
    simdjson::padded_string input(parameters_json);
    if (input.data() == nullptr) {
      error.Set("", "could not allocate parameter input");
      return ErrorCode::kResourceExhausted;
    }
    simdjson::dom::parser parser;
    if (const auto result = parser.allocate(input.size());
        result != simdjson::SUCCESS) {
      return ParseError(result, "", error);
    }
    simdjson::dom::element root;
    if (const auto result = parser.parse(input).get(root);
        result != simdjson::SUCCESS) {
      return ParseError(result, "", error);
    }
    simdjson::dom::object object;
    if (const auto result = root.get_object().get(object);
        result != simdjson::SUCCESS) {
      return ParseError(result, "", error);
    }

    std::string_view venue;
    std::string_view symbol;
    bool has_venue = false;
    bool has_symbol = false;
    for (const auto field : object) {
      if (field.key == "venue") {
        if (has_venue) {
          error.Set(field.key, "duplicate parameter");
          return ErrorCode::kInvalidArgument;
        }
        if (const auto result = field.value.get_string().get(venue);
            result != simdjson::SUCCESS) {
          return ParseError(result, field.key, error);
        }
        has_venue = true;
      } else if (field.key == "symbol") {
        if (has_symbol) {
          error.Set(field.key, "duplicate parameter");
          return ErrorCode::kInvalidArgument;
        }
        if (const auto result = field.value.get_string().get(symbol);
            result != simdjson::SUCCESS) {
          return ParseError(result, field.key, error);
        }
        has_symbol = true;
      } else {
        error.Set(field.key, "unknown parameter");
        return ErrorCode::kInvalidArgument;
      }
    }
    if (!has_venue || !has_symbol) {
      error.Set(!has_venue ? "venue" : "symbol",
                "required parameter is missing");
      return ErrorCode::kInvalidArgument;
    }
    if (venue != "SSE" && venue != "SZSE" && venue != "BSE") {
      error.Set("venue", "must be SSE, SZSE, or BSE");
      return ErrorCode::kInvalidArgument;
    }
    InstrumentId instrument_id;
    if (yquant::model::MakeInstrumentId(venue, symbol, instrument_id) !=
        ErrorCode::kOk) {
      error.Set("symbol", "must contain exactly six ASCII digits");
      return ErrorCode::kInvalidArgument;
    }

    std::memcpy(venue_, venue.data(), venue.size());
    venue_size_ = venue.size();
    std::memcpy(symbol_, symbol.data(), symbol.size());
    return ErrorCode::kOk;
  }

 private:
  char venue_[4] = {};
  size_t venue_size_ = 0;
  char symbol_[yquant::model::kSymbolLength] = {};
};

// Observes one feed and verifies the host's cache-before-callback contract.
// Stores copied definitions and counters, never context or event references.
class QuoteObserverStrategy final : public Strategy {
 public:
  QuoteObserverConfig& config() noexcept override { return config_; }

  ErrorCode OnStart(StrategyContext& context) noexcept override {
    if (state_ != State::kIdle || !config_.ready()) {
      return ErrorCode::kInvalidState;
    }
    auto& market_data = context.market_data();
    InstrumentDefinition instrument;
    const auto find_result = market_data.FindInstrument(
        config_.venue(), config_.symbol(), instrument);
    if (find_result != ErrorCode::kOk) {
      return find_result;
    }
    const auto subscribe_result = market_data.SubscribeQuotes(instrument.id);
    if (subscribe_result != ErrorCode::kOk) {
      return subscribe_result;
    }
    instrument_ = instrument;
    state_ = State::kRunning;
    return ErrorCode::kOk;
  }

  ErrorCode OnTick(StrategyContext& context,
                   const QuoteTick& tick) noexcept override {
    if (state_ != State::kRunning) {
      return ErrorCode::kInvalidState;
    }
    const auto& market_data = context.market_data();
    QuoteTick latest;
    if (tick.instrument_id != instrument_.id ||
        !market_data.TryGetLatestQuote(tick.instrument_id, latest) ||
        latest != tick || tick.sequence <= last_sequence_) {
      return ErrorCode::kCorrupt;
    }
    const auto now = context.Now();
    if (now < tick.receive_time_ns) {
      return ErrorCode::kCorrupt;
    }
    if (ticks_ == std::numeric_limits<uint64_t>::max()) {
      return ErrorCode::kResourceExhausted;
    }
    ++ticks_;
    last_sequence_ = tick.sequence;
    const auto age = now - tick.receive_time_ns;
    if (age > max_age_ns_) {
      max_age_ns_ = age;
    }
    if (ticks_ == 1) {
      return WriteReadiness("quote_observer: first quote received\n");
    }
    return ErrorCode::kOk;
  }

  ErrorCode OnStop(StrategyContext&) noexcept override {
    if (state_ != State::kRunning) {
      return ErrorCode::kInvalidState;
    }
    state_ = State::kStopped;
    // Summary formatting is confined to the low-frequency shutdown callback.
    const int written = ::yquant::base::Print(
        stdout, "quote_observer: ticks={} last_sequence={} max_age_ns={}\n",
        ticks_, last_sequence_, max_age_ns_);
    const int flush_result = std::fflush(stdout);
    return written < 0 || flush_result != 0 ? ErrorCode::kIoError
                                            : ErrorCode::kOk;
  }

 private:
  enum class State { kIdle, kRunning, kStopped };

  QuoteObserverConfig config_;
  InstrumentDefinition instrument_;
  State state_ = State::kIdle;
  uint64_t ticks_ = 0;
  uint64_t last_sequence_ = 0;
  uint64_t max_age_ns_ = 0;
};

// A failed creation leaves output null; successful instances are destroyed by
// the same module without requiring a completed startup.
ErrorCode CreateStrategy(Strategy*& output) noexcept {
  output = nullptr;
  auto* strategy = new (std::nothrow) QuoteObserverStrategy;
  if (strategy == nullptr) {
    return ErrorCode::kResourceExhausted;
  }
  output = strategy;
  return ErrorCode::kOk;
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
