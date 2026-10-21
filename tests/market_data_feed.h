#ifndef YQUANT_TESTS_MARKET_DATA_FEED_H_
#define YQUANT_TESTS_MARKET_DATA_FEED_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "strategyd/market_data_config.h"
#include "yquant/base/json_config_document.h"
#include "yquant/md/mock_quote_source.h"
#include "yquant/md/quote_codec.h"
#include "yquant/md/shm_broadcast.h"

namespace yquant::testing {
// Supplies real SHM traffic to process tests using the reserved test endpoint.
// Production strategyd receives exactly the same interface as with marketd.
// Other endpoints and invalid configurations are left for the child to handle.
class MarketDataFeed {
 public:
  MarketDataFeed() = default;
  MarketDataFeed(const MarketDataFeed&) = delete;
  MarketDataFeed& operator=(const MarketDataFeed&) = delete;
  ~MarketDataFeed() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    (void)source_.Stop();
    publisher_.Close();
    if (directory_[0]) {
      std::error_code error;
      std::filesystem::remove_all(directory_, error);
    }
  }
  // Replaces only the test endpoint with a per-child stream, preserving all
  // schema/source fields. Owned resources survive until after the child exits.
  int Prepare(std::vector<std::string>& arguments) {
    const auto option =
        std::find(arguments.begin(), arguments.end(), "--market-data-config");
    if (option == arguments.end() || option + 1 == arguments.end()) return 0;
    strategyd::MarketDataConfigLoader loader;
    if (!loader.Load((option + 1)->c_str()).ok()) return 0;
    const auto& host = loader.config();
    constexpr std::string_view kEndpoint = "/tmp/yquant-test-feed";
    if (std::string_view(host.runtime_directory) != kEndpoint) return 0;
    char pattern[] = "/tmp/yquant-test-feed-XXXXXX";
    if (!::mkdtemp(pattern)) return errno;
    std::snprintf(directory_, sizeof(directory_), "%s", pattern);
    base::JsonConfigDocument document;
    if (!document.Load((option + 1)->c_str()).ok()) return EINVAL;
    std::string json(document.source());
    const auto position = json.find(kEndpoint);
    if (position == std::string::npos) return EINVAL;
    json.replace(position, kEndpoint.size(), directory_);
    config_path_ = std::string(directory_) + "/receiver.json";
    auto* file = std::fopen(config_path_.c_str(), "wb");
    if (!file) return errno;
    const bool written =
        std::fwrite(json.data(), 1, json.size(), file) == json.size();
    const int closed = std::fclose(file);
    if (!written || closed) return EIO;
    if (host.instrument_count > md::kMaxMockQuoteInstruments) return EINVAL;
    md::MockQuoteConfig config;
    config.source_id = host.source_id;
    config.interval_ns =
        std::max<std::uint64_t>(1000000, host.poll_interval_ns);
    config.instrument_count = host.instrument_count;
    for (std::size_t i = 0; i < config.instrument_count; ++i) {
      auto& item = config.instruments[i];
      item.instrument = host.instruments[i].instrument;
      item.bid_price = 1000;
      item.ask_price = 1001;
      item.bid_qty = 100;
      item.ask_qty = 200;
      item.price_step = 1;
    }
    if (!source_.Prepare(config).ok() ||
        !publisher_
             .Create({{directory_, host.stream}, 4096, md::kQuoteFrameSize})
             .ok())
      return EINVAL;
    *(option + 1) = config_path_;
    thread_ = std::thread([this] {
      std::uint64_t next_tick = 0;
      bool started = false;
      while (!stop_.load()) {
        const auto now = Now();
        if (now >= next_tick) {
          (void)publisher_.Tick();
          next_tick = now + 250000000;
        }
        if (!started) {
          if (!receiving_.load()) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
          }
          if (!source_.Start(now).ok()) return;
          started = true;
        }
        model::QuoteTick quote;
        const auto result = source_.Poll(now, quote);
        if (result.state == md::MockQuotePollState::kEvent) {
          md::QuoteFrame frame;
          if (!md::EncodeQuote(quote, frame).ok() ||
              !publisher_.Publish(md::QuoteEvent(quote, frame)).ok())
            return;
        } else if (result.state == md::MockQuotePollState::kError)
          return;
        else
          std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    });
    return 0;
  }

  // Called after observing the child's Running message: the receiver has
  // attached at Live, so its first delivered source sequence is deterministic.
  void Release() { receiving_.store(true); }

 private:
  static std::uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  char directory_[64] = {};
  std::string config_path_;
  md::MockQuoteSource source_;
  md::ShmPublisher publisher_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> receiving_{false};
  std::thread thread_;
};
}  // namespace yquant::testing
#endif  // YQUANT_TESTS_MARKET_DATA_FEED_H_
