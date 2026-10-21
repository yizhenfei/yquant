#ifndef YQUANT_MD_MOCK_QUOTE_CONFIG_H_
#define YQUANT_MD_MOCK_QUOTE_CONFIG_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "yquant/base/config_error.h"
#include "yquant/model/instrument.h"

namespace yquant::md {

inline constexpr std::size_t kMaxMockQuoteInstruments = 64;

// Owns one validated instrument and its deterministic quote seed.
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
struct MockQuoteConfig {
  model::SourceId source_id;
  std::uint64_t interval_ns = 0;
  std::size_t instrument_count = 0;
  std::array<MockInstrumentConfig, kMaxMockQuoteInstruments> instruments;
};

// Validates bounded seeds and price/step arithmetic without copying config.
[[nodiscard]] base::Status ValidateMockQuoteConfig(
    const MockQuoteConfig& config, base::ConfigError& error) noexcept;

// Strict producer configuration, loaded only during initialization. Catalog
// references use cwd; successful values own all definitions and quote seeds.
class MockQuoteConfigLoader {
 public:
  // Failure clears config and retains owned diagnostics until the next Load.
  [[nodiscard]] base::Status Load(const char* path) noexcept;
  const MockQuoteConfig& config() const noexcept { return config_; }
  const base::ConfigError& error() const noexcept { return error_; }

 private:
  MockQuoteConfig config_;
  base::ConfigError error_;
};

}  // namespace yquant::md

#endif  // YQUANT_MD_MOCK_QUOTE_CONFIG_H_
