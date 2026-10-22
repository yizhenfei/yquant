#include "strategyd/shm_market_data_source.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "base/internal/shared_memory_mapping.h"
#include "base/internal/shm_broadcast_layout.h"
#include "base/internal/shm_stream_registry.h"
#include "gtest/gtest.h"
#include "yquant/md/quote_codec.h"

namespace yquant::strategyd {
namespace {

// Each test owns a private runtime directory and both sides of one stream.
class ShmMarketDataSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NE(mkdtemp(directory_), nullptr);
    config_.adapter = md::MarketDataAdapter::kShm;
    config_.source_id = {1};
    std::strcpy(config_.runtime_directory, directory_);
    std::strcpy(config_.stream, "quotes");
    config_.instrument_count = 2;
    for (std::size_t i = 0; i < config_.instrument_count; ++i) {
      auto& instrument = config_.instruments[i].instrument;
      instrument.venue = model::Venue::kSse;
      const auto* symbol = i == 0 ? "600000" : "600001";
      ASSERT_EQ(model::MakeInstrumentId("SSE", symbol, instrument.id),
                base::ErrorCode::kOk);
      std::memcpy(instrument.symbol, symbol, 6);
      instrument.price_decimals = 2;
    }
  }

  void TearDown() override {
    publisher_.Close();
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
    EXPECT_FALSE(error) << error.message();
  }

  void StartPublisher(std::size_t capacity = 8) {
    const md::PublishConfig config{
        {directory_, "quotes"}, capacity, md::kQuoteFrameSize};
    ASSERT_TRUE(publisher_.Create(config).ok());
  }

  model::QuoteTick Quote(std::size_t instrument = 0,
                         std::uint64_t sequence = 1) {
    model::QuoteTick quote;
    quote.source_id = config_.source_id;
    quote.instrument_id = config_.instruments[instrument].instrument.id;
    quote.sequence = sequence;
    quote.event_time_ns = 123456789;
    // This must never cross into the receiver process's monotonic domain.
    quote.receive_time_ns = UINT64_MAX;
    quote.bid_price = 1000;
    quote.bid_qty = 100;
    quote.ask_price = 1001;
    quote.ask_qty = 200;
    quote.flags = model::kKnownQuoteFlags;
    return quote;
  }

  void Publish(model::QuoteTick quote) {
    md::QuoteFrame frame;
    ASSERT_TRUE(md::EncodeQuote(quote, frame).ok());
    ASSERT_TRUE(publisher_.Publish(md::QuoteEvent(quote, frame)).ok());
  }

  char directory_[64] = "/tmp/yquant-shm-source-XXXXXX";
  md::MarketDataConfig config_;
  md::ShmPublisher publisher_;
};

TEST_F(ShmMarketDataSourceTest,
       PrepareDefersAttachAndStartReadsOnlyLiveQuotes) {
  ShmMarketDataSource source;
  const auto id = config_.instruments[0].instrument.id;
  ASSERT_TRUE(source.Prepare(config_).ok());
  EXPECT_FALSE(source.Start(std::span(&id, 1), 5).ok());
  EXPECT_NE(source.error_detail()[0], '\0');
  StartPublisher();
  Publish(Quote(0, 1));
  ASSERT_TRUE(source.Start(std::span(&id, 1), 5).ok());
  model::QuoteTick output;
  output.sequence = 999;
  const auto unchanged = output;
  EXPECT_EQ(source.Poll(5, output).state, MarketDataPollState::kIdle);
  EXPECT_EQ(output, unchanged);
  const auto expected = Quote(0, 2);
  // Input objects and subscriptions may be destroyed immediately after Start.
  config_ = {};
  Publish(expected);
  ASSERT_EQ(source.Poll(7, output).state, MarketDataPollState::kEvent);
  EXPECT_EQ(output.sequence, 2U);
  EXPECT_EQ(output.event_time_ns, expected.event_time_ns);
  EXPECT_EQ(output.receive_time_ns, 7U);
  EXPECT_TRUE(source.Stop().ok());
}

TEST_F(ShmMarketDataSourceTest, ReadersFilterTheirIndependentSubscriptions) {
  StartPublisher();
  ShmMarketDataSource first;
  ShmMarketDataSource second;
  ASSERT_TRUE(first.Prepare(config_).ok());
  ASSERT_TRUE(second.Prepare(config_).ok());
  const auto first_id = config_.instruments[0].instrument.id;
  const auto second_id = config_.instruments[1].instrument.id;
  ASSERT_TRUE(first.Start(std::span(&first_id, 1), 0).ok());
  ASSERT_TRUE(second.Start(std::span(&second_id, 1), 0).ok());
  Publish(Quote(1));
  Publish(Quote(0));
  model::QuoteTick output;
  ASSERT_EQ(first.Poll(10, output).state, MarketDataPollState::kEvent);
  EXPECT_EQ(output.instrument_id, first_id);
  ASSERT_EQ(second.Poll(20, output).state, MarketDataPollState::kEvent);
  EXPECT_EQ(output.instrument_id, second_id);
  EXPECT_EQ(first.Poll(30, output).state, MarketDataPollState::kIdle);
  EXPECT_EQ(second.Poll(30, output).state, MarketDataPollState::kIdle);
}

TEST_F(ShmMarketDataSourceTest, FiltersEventTypesBeforeDecodingQuotes) {
  StartPublisher();
  ShmMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config_).ok());
  auto id = config_.instruments[0].instrument.id;
  ASSERT_TRUE(source.Start(std::span(&id, 1), 0).ok());
  // Start owns its selection after the borrowed input is changed.
  id = config_.instruments[1].instrument.id;
  md::EventHeader header;
  header.instrument_id = config_.instruments[0].instrument.id.value;
  header.type = 99;
  ASSERT_TRUE(publisher_.Publish({header, {}}).ok());
  model::QuoteTick output = Quote(0, 999);
  const auto unchanged = output;
  EXPECT_EQ(source.Poll(10, output).state, MarketDataPollState::kIdle);
  EXPECT_EQ(output, unchanged);
  Publish(Quote(0, 2));
  ASSERT_EQ(source.Poll(20, output).state, MarketDataPollState::kEvent);
  EXPECT_EQ(output.sequence, 2U);
  EXPECT_EQ(output.instrument_id, config_.instruments[0].instrument.id);
}

TEST_F(ShmMarketDataSourceTest, ReportsGapWithoutSkippingOrChangingOutput) {
  StartPublisher(2);
  ShmMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config_).ok());
  const auto id = config_.instruments[0].instrument.id;
  ASSERT_TRUE(source.Start(std::span(&id, 1), 0).ok());
  for (std::uint64_t sequence = 1; sequence <= 3; ++sequence)
    Publish(Quote(0, sequence));
  model::QuoteTick output = Quote(0, 999);
  const auto unchanged = output;
  EXPECT_EQ(source.Poll(10, output).state, MarketDataPollState::kError);
  EXPECT_EQ(output, unchanged);
  EXPECT_FALSE(source.next_deadline());
  EXPECT_NE(std::string(source.error_detail()).find("gap"), std::string::npos);
  const std::string detail = source.error_detail();
  EXPECT_EQ(source.Poll(20, output).state, MarketDataPollState::kError);
  EXPECT_TRUE(source.Stop().ok());
  EXPECT_STREQ(source.error_detail(), detail.c_str());
  EXPECT_TRUE(source.Stop().ok());
}

TEST_F(ShmMarketDataSourceTest, PublisherEndIsTerminal) {
  StartPublisher();
  ShmMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config_).ok());
  const auto id = config_.instruments[0].instrument.id;
  ASSERT_TRUE(source.Start(std::span(&id, 1), 0).ok());
  publisher_.Close();
  model::QuoteTick output;
  EXPECT_EQ(source.Poll(1, output).state, MarketDataPollState::kError);
  EXPECT_NE(std::string(source.error_detail()).find("publisher stopped"),
            std::string::npos);
}

TEST_F(ShmMarketDataSourceTest, InvalidFramePreservesOutputAndStopsDelivery) {
  StartPublisher();
  ShmMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config_).ok());
  const auto id = config_.instruments[0].instrument.id;
  ASSERT_TRUE(source.Start(std::span(&id, 1), 0).ok());
  const auto quote = Quote();
  md::QuoteFrame frame;
  ASSERT_TRUE(md::EncodeQuote(quote, frame).ok());
  auto event = md::QuoteEvent(quote, frame);
  event.payload = event.payload.first(event.payload.size() - 1);
  ASSERT_TRUE(publisher_.Publish(event).ok());
  model::QuoteTick output = Quote(0, 999);
  const auto unchanged = output;
  EXPECT_EQ(source.Poll(1, output).state, MarketDataPollState::kError);
  EXPECT_EQ(output, unchanged);
  EXPECT_NE(std::string(source.error_detail()).find("invalid shm quote"),
            std::string::npos);
}

TEST_F(ShmMarketDataSourceTest, RejectsQuotesFromUnexpectedSource) {
  StartPublisher();
  ShmMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config_).ok());
  const auto id = config_.instruments[0].instrument.id;
  ASSERT_TRUE(source.Start(std::span(&id, 1), 0).ok());
  auto quote = Quote();
  quote.source_id = {2};
  Publish(quote);
  model::QuoteTick output;
  const auto result = source.Poll(1, output);
  EXPECT_EQ(result.state, MarketDataPollState::kError);
  EXPECT_EQ(result.status.code, base::ErrorCode::kCorrupt);
  EXPECT_NE(std::string(source.error_detail()).find("source_id=2"),
            std::string::npos);
}

TEST_F(ShmMarketDataSourceTest,
       EmptyPollIntervalIsRespectedWithoutDelayingBatch) {
  StartPublisher();
  ShmMarketDataSource source;
  config_.poll_interval_ns = 1000;
  ASSERT_TRUE(source.Prepare(config_).ok());
  const auto id = config_.instruments[0].instrument.id;
  ASSERT_TRUE(source.Start(std::span(&id, 1), 100).ok());
  model::QuoteTick output;
  EXPECT_EQ(source.Poll(100, output).state, MarketDataPollState::kIdle);
  EXPECT_EQ(source.next_deadline(), 1100U);
  Publish(Quote(0, 1));
  Publish(Quote(0, 2));
  EXPECT_EQ(source.Poll(1099, output).state, MarketDataPollState::kIdle);
  ASSERT_EQ(source.Poll(1100, output).state, MarketDataPollState::kEvent);
  EXPECT_EQ(output.sequence, 1U);
  EXPECT_EQ(source.next_deadline(), 1100U);
  ASSERT_EQ(source.Poll(1100, output).state, MarketDataPollState::kEvent);
  EXPECT_EQ(output.sequence, 2U);
  EXPECT_EQ(source.Poll(1100, output).state, MarketDataPollState::kIdle);
  EXPECT_EQ(source.next_deadline(), 2100U);
}

TEST_F(ShmMarketDataSourceTest,
       BoundedFilterScanningYieldsToHostThenContinues) {
  StartPublisher(128);
  ShmMarketDataSource source;
  config_.poll_interval_ns = 1000;
  ASSERT_TRUE(source.Prepare(config_).ok());
  const auto id = config_.instruments[0].instrument.id;
  ASSERT_TRUE(source.Start(std::span(&id, 1), 0).ok());
  for (std::uint64_t sequence = 1; sequence <= 65; ++sequence)
    Publish(Quote(1, sequence));
  Publish(Quote(0));
  model::QuoteTick output;
  EXPECT_EQ(source.Poll(100, output).state, MarketDataPollState::kIdle);
  EXPECT_EQ(source.next_deadline(), 100U);
  ASSERT_EQ(source.Poll(100, output).state, MarketDataPollState::kEvent);
  EXPECT_EQ(output.instrument_id, id);
}

TEST_F(ShmMarketDataSourceTest,
       MaintenanceDrainsCommittedQuotesBeforeStaleError) {
  StartPublisher();
  ShmMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config_).ok());
  const auto id = config_.instruments[0].instrument.id;
  ASSERT_TRUE(source.Start(std::span(&id, 1), 0).ok());
  Publish(Quote());
  auto manifest = base::internal::Discover({directory_, "quotes"});
  ASSERT_TRUE(manifest);
  base::internal::SharedMemoryMapping mapping;
  ASSERT_TRUE(mapping.Open(manifest->shm_name).ok());
  auto* control = reinterpret_cast<base::internal::ShmBroadcastControl*>(
      mapping.memory().data());
  control->heartbeat_ns.store(0, std::memory_order_release);
  EXPECT_EQ(source.next_service_deadline(), kShmHealthCheckIntervalNs);
  ASSERT_TRUE(source.Service(kShmHealthCheckIntervalNs - 1).ok());
  ASSERT_TRUE(source.Service(kShmHealthCheckIntervalNs).ok());
  model::QuoteTick output;
  ASSERT_EQ(source.Poll(kShmHealthCheckIntervalNs, output).state,
            MarketDataPollState::kEvent);
  EXPECT_EQ(output.sequence, 1U);
  EXPECT_EQ(source.Poll(kShmHealthCheckIntervalNs, output).state,
            MarketDataPollState::kError);
  EXPECT_NE(std::string(source.error_detail()).find("heartbeat is stale"),
            std::string::npos);
  EXPECT_FALSE(source.next_service_deadline());
}

TEST_F(ShmMarketDataSourceTest, MaintenanceDetectsRestartBeforeOldDelivery) {
  StartPublisher();
  ShmMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config_).ok());
  const auto id = config_.instruments[0].instrument.id;
  ASSERT_TRUE(source.Start(std::span(&id, 1), 0).ok());
  Publish(Quote());
  publisher_.Close();
  StartPublisher();
  const auto status = source.Service(kShmHealthCheckIntervalNs);
  EXPECT_EQ(status.code, base::ErrorCode::kInvalidState);
  EXPECT_NE(std::string(source.error_detail()).find("generation changed"),
            std::string::npos);
  model::QuoteTick output;
  EXPECT_EQ(source.Poll(kShmHealthCheckIntervalNs, output).state,
            MarketDataPollState::kError);
}

TEST_F(ShmMarketDataSourceTest, MaintenanceDetectsFailureWithoutSubscriptions) {
  StartPublisher();
  ShmMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config_).ok());
  ASSERT_TRUE(source.Start({}, 0).ok());
  EXPECT_FALSE(source.next_deadline());
  EXPECT_TRUE(source.next_service_deadline());
  publisher_.Close();
  ASSERT_TRUE(source.Service(kShmHealthCheckIntervalNs).ok());
  EXPECT_EQ(source.next_deadline(), kShmHealthCheckIntervalNs);
  model::QuoteTick output;
  EXPECT_EQ(source.Poll(kShmHealthCheckIntervalNs, output).state,
            MarketDataPollState::kError);
}

TEST_F(ShmMarketDataSourceTest, RejectsInvalidStateAndSubscriptions) {
  ShmMarketDataSource source;
  model::QuoteTick output = Quote(0, 99);
  const auto unchanged = output;
  EXPECT_EQ(source.Poll(1, output).state, MarketDataPollState::kError);
  EXPECT_EQ(output, unchanged);
  EXPECT_FALSE(source.Start({}, 0).ok());
  ASSERT_TRUE(source.Prepare(config_).ok());
  EXPECT_FALSE(source.Prepare(config_).ok());
  const auto id = config_.instruments[0].instrument.id;
  const std::array duplicate = {id, id};
  EXPECT_EQ(source.Start(duplicate, 0).code, base::ErrorCode::kInvalidArgument);
  const std::array unknown = {model::InstrumentId{1}};
  EXPECT_EQ(source.Start(unknown, 0).code, base::ErrorCode::kInvalidArgument);
  StartPublisher();
  ASSERT_TRUE(source.Start({}, 0).ok());
  EXPECT_FALSE(source.next_deadline());
  EXPECT_EQ(source.Poll(UINT64_MAX, output).state, MarketDataPollState::kIdle);
  EXPECT_EQ(output, unchanged);
  EXPECT_TRUE(source.Stop().ok());
}

}  // namespace
}  // namespace yquant::strategyd
