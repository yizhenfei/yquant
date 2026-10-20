#ifndef YQUANT_STRATEGYD_MARKET_DATA_CONFIG_H_
#define YQUANT_STRATEGYD_MARKET_DATA_CONFIG_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "yquant/base/status.h"
#include "yquant/model/instrument.h"
#include "yquant/strategy/strategy_config.h"

namespace yquant::strategyd {

inline constexpr std::size_t kMaxMarketDataInstruments = 64;

// Owns the initial two-sided quote and bounded alternating price increment.
struct MockInstrumentConfig {
  model::InstrumentDefinition instrument;
  std::int64_t bid_price = 0;
  std::int64_t bid_qty = 0;
  std::int64_t ask_price = 0;
  std::int64_t ask_qty = 0;
  std::int64_t price_step = 0;
};

// Fixed-capacity, self-contained configuration. Only the first instrument_count
// elements are active. Stored values remain usable after JSON is released.
struct MarketDataConfig {
  model::SourceId source_id;
  std::uint64_t interval_ns = 0;
  std::size_t instrument_count = 0;
  std::array<MockInstrumentConfig, kMaxMarketDataInstruments> instruments;
};

// Validates typed configuration too, so sources do not rely on JSON-only
// checks. On failure error owns a field path/reason; config is never modified.
[[nodiscard]] base::Status ValidateMarketDataConfig(
    const MarketDataConfig& config, strategy::ConfigError& error) noexcept;

// Loads strict mock-source JSON. No parser, source views or heap allocations
// survive Load(). Methods are serialized by the owning host thread.
class MarketDataConfigLoader {
 public:
  // Replaces the previous configuration. Failure leaves config() empty and
  // error() owns diagnostics until the next call. Borrows path during the call.
  [[nodiscard]] base::Status Load(const char* path) noexcept;
  const MarketDataConfig& config() const noexcept { return config_; }
  const strategy::ConfigError& error() const noexcept { return error_; }

 private:
  MarketDataConfig config_;
  strategy::ConfigError error_;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_MARKET_DATA_CONFIG_H_
