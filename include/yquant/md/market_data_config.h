#ifndef YQUANT_MD_MARKET_DATA_CONFIG_H_
#define YQUANT_MD_MARKET_DATA_CONFIG_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "yquant/base/config_error.h"
#include "yquant/base/status.h"
#include "yquant/model/instrument.h"

namespace yquant::md {

inline constexpr std::size_t kMaxMarketDataInstruments = 64;
inline constexpr std::size_t kMaxRuntimeDirectoryLength = 4095;
inline constexpr std::size_t kMaxStreamNameLength = 64;

enum class MarketDataAdapter { kShm };

// One instrument definition in the host receive scope.
struct MarketDataInstrumentConfig {
  model::InstrumentDefinition instrument;
};

// Fixed-capacity, self-contained configuration. Only the first instrument_count
// elements are active. Stored values remain usable after JSON is released.
struct MarketDataConfig {
  model::SourceId source_id;
  MarketDataAdapter adapter = MarketDataAdapter::kShm;
  // SHM endpoint values are owned, NUL-terminated strings. A zero poll interval
  // continuously polls; positive intervals wait only after an empty read.
  char runtime_directory[kMaxRuntimeDirectoryLength + 1] = {};
  char stream[kMaxStreamNameLength + 1] = {};
  std::uint64_t poll_interval_ns = 0;
  std::size_t instrument_count = 0;
  std::array<MarketDataInstrumentConfig, kMaxMarketDataInstruments> instruments;
};

// Validates only source identity and instrument definitions, for host services
// and dictionary-only consumers that do not open a receive endpoint.
[[nodiscard]] base::Status ValidateMarketDataScope(
    const MarketDataConfig& config, base::ConfigError& error) noexcept;

// Validates typed configuration too, so sources do not rely on JSON-only
// checks. On failure error owns a field path/reason; config is never modified.
[[nodiscard]] base::Status ValidateMarketDataConfig(
    const MarketDataConfig& config, base::ConfigError& error) noexcept;

// Loads strict SHM receive JSON and resolves its selected instruments from
// the referenced catalog. Relative catalog paths use the process working
// directory. No parser, source views or temporary catalog storage survive
// Load(). Methods are serialized by the owning host thread.
class MarketDataConfigLoader {
 public:
  // Replaces the previous configuration. Failure leaves config() empty and
  // error() owns diagnostics until the next call. Borrows path during the call.
  [[nodiscard]] base::Status Load(const char* path) noexcept;
  const MarketDataConfig& config() const noexcept { return config_; }
  const base::ConfigError& error() const noexcept { return error_; }

 private:
  MarketDataConfig config_;
  base::ConfigError error_;
};

}  // namespace yquant::md

#endif  // YQUANT_MD_MARKET_DATA_CONFIG_H_
