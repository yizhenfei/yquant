#include "yquant/market/sources.h"

#include <stddef.h>
#include <stdint.h>

#include <cstddef>
#include <string>

#include "gtest/gtest.h"
#include "market/pipeline_test_support.h"
#include "yquant/market/quote_codec.h"

namespace yquant::market {
namespace {

TEST(MockSourceTest, SubscribesAllInstrumentsAndUsesLocalReceiveTime) {
  const auto config = testing::MockConfig(2);
  MockSource source;
  ASSERT_TRUE(source.Init(config, 50).ok());
  auto quote = testing::Quote(999);
  const auto unchanged = quote;
  EXPECT_EQ(source.Poll(50, quote).state, PollState::kIdle);
  EXPECT_EQ(quote, unchanged);
  for (size_t i = 0; i < 2; ++i) {
    ASSERT_EQ(source.Poll(1000050, quote).state, PollState::kEvent);
    EXPECT_EQ(quote.instrument_id, config.instruments[i].instrument.id);
    EXPECT_EQ(quote.source_id, config.source_id);
    EXPECT_EQ(quote.sequence, 1U);
    EXPECT_EQ(quote.receive_time_ns, 1000050U);
  }
  source.Close();
  const auto last = quote;
  EXPECT_EQ(source.Poll(2000050, quote).state, PollState::kError);
  EXPECT_EQ(quote, last);
}

TEST(ShmPipelineTest, BroadcastPreservesMetadataAndRestampsLocalClock) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const market::Endpoint endpoint{directory.path(), "quotes"};
  ShmPublisher publisher;
  ASSERT_TRUE(publisher.Init({endpoint, 16, 80}).ok());
  ASSERT_TRUE(publisher.Service(0).ok());
  ShmSource first;
  ShmSource second;
  ASSERT_TRUE(first.Init(endpoint).ok());
  ASSERT_TRUE(second.Init(endpoint).ok());
  auto quote = testing::Quote(123);
  ASSERT_TRUE(publisher.Publish(quote).ok());
  model::QuoteTick out;
  ASSERT_EQ(first.Poll(456, out).state, PollState::kEvent)
      << first.error_detail();
  quote.receive_time_ns = 456;
  EXPECT_EQ(out, quote);
  ASSERT_EQ(second.Poll(789, out).state, PollState::kEvent);
  quote.receive_time_ns = 789;
  EXPECT_EQ(out, quote);
  const auto unchanged = out;
  EXPECT_EQ(second.Poll(790, out).state, PollState::kIdle);
  EXPECT_EQ(out, unchanged);
}

TEST(ShmPipelineTest, PublishesQuotesAndEventsThroughOnePublisher) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const market::Endpoint endpoint{directory.path(), "quotes"};
  ShmPublisher publisher;
  ASSERT_TRUE(publisher.Init({endpoint, 16, 80}).ok());
  Publisher& output = publisher;
  ASSERT_TRUE(output.Service(0).ok());
  ShmSource source;
  ASSERT_TRUE(source.Init(endpoint).ok());

  auto first = testing::Quote(1);
  ASSERT_TRUE(output.Publish(first).ok());
  auto second = testing::Quote(2);
  QuoteFrame frame;
  ASSERT_TRUE(EncodeQuote(second, frame).ok());
  uint64_t sequence = 0;
  ASSERT_TRUE(publisher.Publish(QuoteEvent(second, frame), &sequence).ok());
  EXPECT_EQ(sequence, 2U);

  model::QuoteTick out;
  ASSERT_EQ(source.Poll(100, out).state, PollState::kEvent);
  first.receive_time_ns = 100;
  EXPECT_EQ(out, first);
  ASSERT_EQ(source.Poll(101, out).state, PollState::kEvent);
  second.receive_time_ns = 101;
  EXPECT_EQ(out, second);

  ASSERT_TRUE(output.Finish().ok());
  ASSERT_TRUE(output.Finish().ok());
  EXPECT_FALSE(output.Publish(first).ok());
  EXPECT_FALSE(output.Service(102).ok());
  ShmPublisher replacement;
  EXPECT_TRUE(replacement.Init({endpoint, 16, 80}).ok());
}

TEST(ShmPipelineTest, GapIsTerminalAndDiagnosticSurvivesClose) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const market::Endpoint endpoint{directory.path(), "quotes"};
  ShmPublisher publisher;
  ASSERT_TRUE(publisher.Init({endpoint, 2, 80}).ok());
  ShmSource source;
  ASSERT_TRUE(source.Init(endpoint).ok());
  for (uint64_t i = 1; i <= 3; ++i) {
    ASSERT_TRUE(publisher.Publish(testing::Quote(i)).ok());
  }
  auto out = testing::Quote(999);
  const auto unchanged = out;
  auto result = source.Poll(100, out);
  ASSERT_EQ(result.state, PollState::kError);
  EXPECT_EQ(result.status.code, base::ErrorCode::kCorrupt);
  EXPECT_EQ(out, unchanged);
  EXPECT_NE(std::strstr(source.error_detail(), "gap"), nullptr);
  const std::string detail = source.error_detail();
  source.Close();
  EXPECT_EQ(source.error_detail(), detail);
  result = source.Poll(101, out);
  EXPECT_EQ(result.state, PollState::kError);
  EXPECT_EQ(result.status.code, base::ErrorCode::kCorrupt);
  EXPECT_EQ(out, unchanged);
}

TEST(ShmPipelineTest, CorruptFramesAreTerminalWithoutChangingOutput) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const market::Endpoint endpoint{directory.path(), "quotes"};
  market::ShmPublisher publisher;
  ASSERT_TRUE(publisher.Create({endpoint, 16, 80}).ok());
  ShmSource source;
  ASSERT_TRUE(source.Init(endpoint).ok());
  auto quote = testing::Quote();
  market::QuoteFrame frame;
  ASSERT_TRUE(market::EncodeQuote(quote, frame).ok());
  frame[0] ^= std::byte{1};
  ASSERT_TRUE(publisher.Publish(market::QuoteEvent(quote, frame)).ok());
  auto out = testing::Quote(999);
  const auto unchanged = out;
  EXPECT_EQ(source.Poll(10, out).state, PollState::kError);
  EXPECT_EQ(out, unchanged);
  EXPECT_NE(std::strstr(source.error_detail(), "corrupt"), nullptr);
  EXPECT_EQ(source.Poll(11, out).state, PollState::kError);
}

TEST(ShmPipelineTest, GenerationReplacementAndEndAreTerminal) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const market::Endpoint endpoint{directory.path(), "quotes"};
  ShmPublisher publisher;
  ASSERT_TRUE(publisher.Init({endpoint, 16, 80}).ok());
  ShmSource source;
  ASSERT_TRUE(source.Init(endpoint).ok());
  publisher.Close();
  model::QuoteTick out;
  EXPECT_EQ(source.Poll(1, out).state, PollState::kError);
  EXPECT_NE(std::strstr(source.error_detail(), "ended"), nullptr);
  source.Close();
  ASSERT_TRUE(publisher.Init({endpoint, 16, 80}).ok());
  ASSERT_TRUE(source.Init(endpoint).ok());
  EXPECT_EQ(source.Poll(100, out).state, PollState::kIdle);
  publisher.Close();
  ASSERT_TRUE(publisher.Init({endpoint, 16, 80}).ok());
  EXPECT_EQ(source.Poll(250000100, out).state, PollState::kError);
  EXPECT_NE(std::strstr(source.error_detail(), "changed"), nullptr);
}

TEST(ShmPipelineTest, MissingDiscoveryDrainsCommittedEventsBeforeError) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const market::Endpoint endpoint{directory.path(), "quotes"};
  ShmPublisher publisher;
  ASSERT_TRUE(publisher.Init({endpoint, 16, 80}).ok());
  ShmSource source;
  ASSERT_TRUE(source.Init(endpoint).ok());
  ASSERT_TRUE(publisher.Publish(testing::Quote(1)).ok());
  ASSERT_TRUE(publisher.Publish(testing::Quote(2)).ok());
  ASSERT_TRUE(::yquant::testing::files::RemoveFile(
      ::yquant::testing::files::FilePath(directory.File("quotes.manifest"))));
  model::QuoteTick out;
  ASSERT_EQ(source.Poll(1, out).state, PollState::kEvent);
  EXPECT_EQ(out.sequence, 1U);
  ASSERT_EQ(source.Poll(2, out).state, PollState::kEvent);
  EXPECT_EQ(out.sequence, 2U);
  const auto unchanged = out;
  EXPECT_EQ(source.Poll(3, out).state, PollState::kError);
  EXPECT_NE(std::strstr(source.error_detail(), "missing"), nullptr);
  EXPECT_EQ(out, unchanged);
}

TEST(ShmPipelineTest, RejectsInsufficientPayloadCapacityBeforeOpeningInput) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const market::Endpoint endpoint{directory.path(), "quotes"};
  ShmPublisher adapter;
  EXPECT_FALSE(adapter.Init({endpoint, 16, 79}).ok());
  market::ShmPublisher publisher;
  ASSERT_TRUE(publisher.Create({endpoint, 16, 32}).ok());
  ShmSource source;
  EXPECT_FALSE(source.Init(endpoint).ok());
}

}  // namespace
}  // namespace yquant::market
