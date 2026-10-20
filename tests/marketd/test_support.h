#ifndef YQUANT_TESTS_MARKETD_TEST_SUPPORT_H_
#define YQUANT_TESTS_MARKETD_TEST_SUPPORT_H_

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "marketd/config.h"
#include "yquant/model/identifiers.h"
#include "yquant/model/instrument.h"
#include "yquant/model/quote_tick.h"

namespace yquant::marketd::testing {

// Isolates on-disk discovery and stable lock files for each test.
class RuntimeDirectory {
 public:
  RuntimeDirectory() { valid_ = mkdtemp(path_) != nullptr; }
  ~RuntimeDirectory() {
    if (valid_) {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }
  }
  RuntimeDirectory(const RuntimeDirectory&) = delete;
  RuntimeDirectory& operator=(const RuntimeDirectory&) = delete;
  bool valid() const { return valid_; }
  const char* path() const { return path_; }
  std::string File(std::string_view name) const {
    return std::string(path_) + "/" + std::string(name);
  }
  bool Write(std::string_view name, std::string_view contents) const {
    auto* file = std::fopen(File(name).c_str(), "wb");
    if (file == nullptr) return false;
    const bool written = std::fwrite(contents.data(), 1, contents.size(),
                                     file) == contents.size();
    return std::fclose(file) == 0 && written;
  }

 private:
  char path_[64] = "/tmp/yquant-marketd-XXXXXX";
  bool valid_ = false;
};

inline strategyd::MarketDataConfig MockConfig(std::size_t count = 1) {
  strategyd::MarketDataConfig config;
  config.source_id = {42};
  config.interval_ns = 1000000;
  config.instrument_count = count;
  for (std::size_t i = 0; i < count; ++i) {
    auto& item = config.instruments[i];
    item.instrument.venue = model::Venue::kSse;
    char symbol[model::kSymbolLength + 1];
    std::snprintf(symbol, sizeof(symbol), "%0*u",
                  static_cast<int>(model::kSymbolLength),
                  600000U + static_cast<unsigned>(i));
    EXPECT_EQ(model::MakeInstrumentId("SSE", symbol, item.instrument.id),
              base::ErrorCode::kOk);
    std::memcpy(item.instrument.symbol, symbol, model::kSymbolLength);
    item.instrument.price_decimals = 2;
    item.bid_price = 1000;
    item.ask_price = 1001;
    item.bid_qty = 100;
    item.ask_qty = 200;
    item.price_step = 1;
  }
  return config;
}

inline model::QuoteTick Quote(std::uint64_t sequence = 1) {
  const auto config = MockConfig();
  model::QuoteTick quote;
  quote.source_id = config.source_id;
  quote.instrument_id = config.instruments[0].instrument.id;
  quote.sequence = sequence;
  quote.event_time_ns = 123456789;
  quote.receive_time_ns = 888;
  quote.bid_price = 1000;
  quote.ask_price = 1001;
  quote.bid_qty = 100;
  quote.ask_qty = 200;
  quote.flags = model::kKnownQuoteFlags;
  return quote;
}

inline PublisherConfig ShmOutput(const char* directory, const char* stream) {
  PublisherConfig output;
  output.adapter = Adapter::kShm;
  std::snprintf(output.runtime_directory, sizeof(output.runtime_directory),
                "%s", directory);
  std::snprintf(output.stream, sizeof(output.stream), "%s", stream);
  output.capacity = 128;
  return output;
}

inline MarketdConfig LocalConfig(const char* directory) {
  MarketdConfig config;
  config.source.adapter = Adapter::kMock;
  config.source.mock = MockConfig();
  config.publishers[0] = ShmOutput(directory, "quotes");
  config.publisher_count = 1;
  return config;
}

inline constexpr std::string_view kMockJson = R"({
  "source_id": 42,
  "adapter": "mock",
  "interval_ms": 1,
  "instruments": [{"venue":"SSE","symbol":"600000","price_decimals":2,
    "bid_price":1000,"bid_qty":100,"ask_price":1001,"ask_qty":200,
    "price_step":1}]
})";

}  // namespace yquant::marketd::testing

#endif  // YQUANT_TESTS_MARKETD_TEST_SUPPORT_H_
