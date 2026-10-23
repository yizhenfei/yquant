#include "marketd/adapters.h"

#include <stddef.h>
#include <stdint.h>

#include <cstddef>
#include <string>

#include "gtest/gtest.h"
#include "test_support.h"
#include "yquant/md/quote_codec.h"

namespace yquant::marketd {
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

TEST(ShmAdapterTest, BroadcastPreservesMetadataAndRestampsLocalClock) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const md::Endpoint endpoint{directory.path(), "quotes"};
  ShmPublisherAdapter publisher;
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

TEST(ShmAdapterTest, GapIsTerminalAndDiagnosticSurvivesClose) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const md::Endpoint endpoint{directory.path(), "quotes"};
  ShmPublisherAdapter publisher;
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

TEST(ShmAdapterTest, CorruptFramesAreTerminalWithoutChangingOutput) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const md::Endpoint endpoint{directory.path(), "quotes"};
  md::ShmPublisher publisher;
  ASSERT_TRUE(publisher.Create({endpoint, 16, 80}).ok());
  ShmSource source;
  ASSERT_TRUE(source.Init(endpoint).ok());
  auto quote = testing::Quote();
  md::QuoteFrame frame;
  ASSERT_TRUE(md::EncodeQuote(quote, frame).ok());
  frame[0] ^= std::byte{1};
  ASSERT_TRUE(publisher.Publish(md::QuoteEvent(quote, frame)).ok());
  auto out = testing::Quote(999);
  const auto unchanged = out;
  EXPECT_EQ(source.Poll(10, out).state, PollState::kError);
  EXPECT_EQ(out, unchanged);
  EXPECT_NE(std::strstr(source.error_detail(), "corrupt"), nullptr);
  EXPECT_EQ(source.Poll(11, out).state, PollState::kError);
}

TEST(ShmAdapterTest, GenerationReplacementAndEndAreTerminal) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const md::Endpoint endpoint{directory.path(), "quotes"};
  ShmPublisherAdapter publisher;
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

TEST(ShmAdapterTest, MissingDiscoveryDrainsCommittedEventsBeforeError) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const md::Endpoint endpoint{directory.path(), "quotes"};
  ShmPublisherAdapter publisher;
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

TEST(ShmAdapterTest, RejectsInsufficientPayloadCapacityBeforeOpeningInput) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const md::Endpoint endpoint{directory.path(), "quotes"};
  ShmPublisherAdapter adapter;
  EXPECT_FALSE(adapter.Init({endpoint, 16, 79}).ok());
  md::ShmPublisher publisher;
  ASSERT_TRUE(publisher.Create({endpoint, 16, 32}).ok());
  ShmSource source;
  EXPECT_FALSE(source.Init(endpoint).ok());
}

}  // namespace
}  // namespace yquant::marketd
