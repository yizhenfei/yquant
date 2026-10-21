#include "marketd/runner.h"

#include <cstring>
#include <limits>
#include <optional>

#include "gtest/gtest.h"
#include "test_support.h"
#include "yquant/md/quote_codec.h"
#include "yquant/md/shm_broadcast.h"

namespace yquant::marketd {
namespace {

// Advances one mock interval per source poll; no wall-clock waiting is needed.
class AdvancingClock final : public strategyd::MonotonicClock {
 public:
  std::uint64_t Now() const noexcept override {
    now_ += 1000000;
    return now_;
  }

 private:
  mutable std::uint64_t now_ = 0;
};

// Requests stop at a selected batch boundary and records any unexpected waits.
class Environment final : public strategyd::EventLoopEnvironment {
 public:
  base::Status CheckStop(bool& out) noexcept override {
    ++checks;
    if (fail) return {base::ErrorCode::kIoError, {}};
    out = checks >= stop_after;
    return {};
  }
  base::Status Wait(std::optional<std::uint64_t>) noexcept override {
    ++waits;
    return {};
  }
  std::size_t checks = 0;
  std::size_t waits = 0;
  std::size_t stop_after = std::numeric_limits<std::size_t>::max();
  bool fail = false;
};

TEST(MarketdRunnerTest, PublishesExactlyLimitToEveryOutputAndClosesDiscovery) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  auto config = testing::LocalConfig(directory.path());
  config.publishers[1] = testing::ShmOutput(directory.path(), "other");
  config.publisher_count = 2;
  MarketdRunner runner;
  ASSERT_TRUE(runner.Init(config, 0).ok()) << runner.error_detail();
  md::ShmReader first;
  md::ShmReader second;
  ASSERT_TRUE(first.Open({directory.path(), "quotes"}).ok());
  ASSERT_TRUE(second.Open({directory.path(), "other"}).ok());
  AdvancingClock clock;
  Environment environment;
  ASSERT_TRUE(runner.Run(clock, environment, 5).ok()) << runner.error_detail();
  EXPECT_EQ(runner.event_count(), 5U);
  EXPECT_EQ(environment.waits, 0U);
  for (std::uint64_t sequence = 1; sequence <= 5; ++sequence) {
    md::EventBuffer first_buffer;
    md::EventBuffer second_buffer;
    ASSERT_EQ(first.TryNext(first_buffer).status, md::ReadStatus::kEvent);
    ASSERT_EQ(second.TryNext(second_buffer).status, md::ReadStatus::kEvent);
    model::QuoteTick quote;
    ASSERT_TRUE(md::DecodeQuoteEvent(first_buffer.view(), quote).ok());
    EXPECT_EQ(quote.sequence, sequence);
    EXPECT_EQ(quote.source_id, config.source.mock.source_id);
    EXPECT_EQ(first_buffer.payload, second_buffer.payload);
    EXPECT_EQ(first_buffer.payload_size, second_buffer.payload_size);
  }
  md::EventBuffer buffer;
  EXPECT_EQ(first.TryNext(buffer).status, md::ReadStatus::kEndOfGeneration);
  md::ShmReader later;
  EXPECT_FALSE(later.Open({directory.path(), "quotes"}).ok());
}

TEST(MarketdRunnerTest, ChecksTerminationAtBoundedBatchBoundaries) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto config = testing::LocalConfig(directory.path());
  MarketdRunner runner;
  ASSERT_TRUE(runner.Init(config, 0).ok());
  AdvancingClock clock;
  Environment environment;
  environment.stop_after = 2;
  ASSERT_TRUE(runner.Run(clock, environment).ok());
  EXPECT_EQ(environment.checks, 2U);
  EXPECT_EQ(runner.event_count(), 64U);
}

TEST(MarketdRunnerTest, ValidatesAllConfigurationBeforeCreatingOutputs) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  auto config = testing::LocalConfig(directory.path());
  config.publishers[1] = testing::ShmOutput(directory.path(), "invalid");
  config.publishers[1].capacity = 3;
  config.publisher_count = 2;
  MarketdRunner runner;
  EXPECT_FALSE(runner.Init(config, 0).ok());
  EXPECT_STREQ(runner.error_stage(), "config");
  EXPECT_FALSE(std::filesystem::exists(directory.File("quotes.lock")));
  md::ShmReader reader;
  EXPECT_FALSE(reader.Open({directory.path(), "quotes"}).ok());
}

TEST(MarketdRunnerTest, PartialOutputInitializationUnwindsAndReleasesLocks) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  md::ShmPublisher existing;
  ASSERT_TRUE(existing.Create({{directory.path(), "taken"}, 16, 80}).ok());
  auto config = testing::LocalConfig(directory.path());
  config.publishers[1] = testing::ShmOutput(directory.path(), "taken");
  config.publisher_count = 2;
  MarketdRunner runner;
  EXPECT_FALSE(runner.Init(config, 0).ok());
  EXPECT_STREQ(runner.error_stage(), "publisher_init");
  md::ShmReader reader;
  EXPECT_FALSE(reader.Open({directory.path(), "quotes"}).ok());
  md::ShmPublisher replacement;
  EXPECT_TRUE(replacement.Create({{directory.path(), "quotes"}, 16, 80}).ok());
}

TEST(MarketdRunnerTest, SignalCheckFailureClosesOutputsAndPreservesReason) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  MarketdRunner runner;
  ASSERT_TRUE(runner.Init(testing::LocalConfig(directory.path()), 0).ok());
  AdvancingClock clock;
  Environment environment;
  environment.fail = true;
  EXPECT_FALSE(runner.Run(clock, environment).ok());
  EXPECT_STREQ(runner.error_stage(), "termination");
  EXPECT_NE(std::strstr(runner.error_detail(), "termination"), nullptr);
  runner.Close();
  EXPECT_STREQ(runner.error_stage(), "termination");
  md::ShmPublisher replacement;
  EXPECT_TRUE(replacement.Create({{directory.path(), "quotes"}, 16, 80}).ok());
}

}  // namespace
}  // namespace yquant::marketd
