#ifndef YQUANT_TESTS_MARKET_PIPELINE_TEST_SUPPORT_H_
#define YQUANT_TESTS_MARKET_PIPELINE_TEST_SUPPORT_H_

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>

#include "file_test_support.h"
#include "gtest/gtest.h"
#include "yquant/base/format.h"
#include "yquant/market/market_data_config.h"
#include "yquant/market/pipeline_config.h"
#include "yquant/model/identifiers.h"
#include "yquant/model/instrument.h"
#include "yquant/model/quote_tick.h"

namespace yquant::market::testing {

// Isolates on-disk discovery, lock files and a shared catalog copy per test.
class RuntimeDirectory {
 public:
  RuntimeDirectory() {
    owns_directory_ = mkdtemp(path_) != nullptr;
    valid_ = owns_directory_ &&
             Copy("instruments.json", YQUANT_TEST_INSTRUMENT_CATALOG_PATH);
  }
  ~RuntimeDirectory() {
    if (owns_directory_) {
      std::error_code error;
      ::yquant::testing::files::RemoveTree(
          ::yquant::testing::files::FilePath(path_), error);
    }
  }
  RuntimeDirectory(const RuntimeDirectory&) = delete;
  RuntimeDirectory& operator=(const RuntimeDirectory&) = delete;
  bool valid() const { return valid_; }
  const char* path() const { return path_; }
  std::string File(std::string_view name) const {
    return std::string(path_) + "/" + std::string(name);
  }
  bool Copy(std::string_view name, const char* source) const {
    std::error_code error;
    return ::yquant::testing::files::OverwriteFile(
        ::yquant::testing::files::FilePath(source),
        ::yquant::testing::files::FilePath(File(name)), error);
  }
  bool Write(std::string_view name, std::string_view contents) const {
    auto* file = std::fopen(File(name).c_str(), "wb");
    if (file == nullptr) {
      return false;
    }
    const bool written = std::fwrite(contents.data(), 1, contents.size(),
                                     file) == contents.size();
    return std::fclose(file) == 0 && written;
  }

 private:
  char path_[64] = "/tmp/yquant-marketd-XXXXXX";
  bool owns_directory_ = false;
  bool valid_ = false;
};

inline market::MockQuoteConfig MockConfig(size_t count = 1) {
  market::MockQuoteConfig config;
  config.source_id = {42};
  config.interval_ns = 1000000;
  config.instrument_count = count;
  for (size_t i = 0; i < count; ++i) {
    auto& item = config.instruments[i];
    item.instrument.venue = model::Venue::kSse;
    char symbol[model::kSymbolLength + 1];
    ::yquant::base::FormatTo(symbol, sizeof(symbol), "{:0{}}",
                             600000U + static_cast<unsigned>(i),
                             static_cast<int>(model::kSymbolLength));
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
// Dictionary-only view for gateway tests; quote seeds stay producer-owned.
inline market::MarketDataConfig ScopeConfig() {
  const auto mock = MockConfig();
  market::MarketDataConfig scope;
  scope.source_id = mock.source_id;
  scope.instrument_count = mock.instrument_count;
  for (size_t i = 0; i < scope.instrument_count; ++i) {
    scope.instruments[i].instrument = mock.instruments[i].instrument;
  }
  return scope;
}

inline model::QuoteTick Quote(uint64_t sequence = 1) {
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
  ::yquant::base::FormatTo(output.runtime_directory,
                           sizeof(output.runtime_directory), "{}", directory);
  ::yquant::base::FormatTo(output.stream, sizeof(output.stream), "{}", stream);
  output.capacity = 128;
  return output;
}

inline PipelineConfig LocalConfig(const char* directory) {
  PipelineConfig config;
  config.source.adapter = Adapter::kMock;
  config.source.mock = MockConfig();
  config.publishers[0] = ShmOutput(directory, "quotes");
  config.publisher_count = 1;
  return config;
}

inline constexpr std::string_view kMockJson = R"({
  "source_id": 42,
  "adapter": "mock",
  "instrument_catalog": "instruments.json",
  "interval_ms": 1,
  "instruments": [{"venue":"SSE","symbol":"600000",
    "bid_price":1000,"bid_qty":100,"ask_price":1001,"ask_qty":200,
    "price_step":1}]
})";

}  // namespace yquant::market::testing

#endif  // YQUANT_TESTS_MARKET_PIPELINE_TEST_SUPPORT_H_
