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
inline constexpr std::size_t kMaxRuntimeDirectoryLength = 4095;
inline constexpr std::size_t kMaxStreamNameLength = 64;

enum class MarketDataAdapter { kMock, kShm };

// Owns one instrument definition and optional mock quote seed. Quote values
// remain zero when the adapter reads SHM instead of generating mock data.
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
  MarketDataAdapter adapter = MarketDataAdapter::kMock;
  std::uint64_t interval_ns = 0;
  // SHM endpoint values are owned, NUL-terminated strings. A zero poll interval
  // continuously polls; positive intervals wait only after an empty read.
  char runtime_directory[kMaxRuntimeDirectoryLength + 1] = {};
  char stream[kMaxStreamNameLength + 1] = {};
  std::uint64_t poll_interval_ns = 0;
  std::size_t instrument_count = 0;
  std::array<MockInstrumentConfig, kMaxMarketDataInstruments> instruments;
};

// Validates typed configuration too, so sources do not rely on JSON-only
// checks. On failure error owns a field path/reason; config is never modified.
[[nodiscard]] base::Status ValidateMarketDataConfig(
    const MarketDataConfig& config, strategy::ConfigError& error) noexcept;

// Loads strict mock/SHM source JSON and resolves its selected instruments from
// the referenced catalog. Relative catalog paths use this config's directory.
// No parser, source views or temporary catalog storage survive Load(). Methods
// are serialized by the owning host thread.
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
