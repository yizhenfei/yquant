#include <array>
#include <cstddef>
#include <thread>

#include "gtest/gtest.h"

#include "market/fixed_quote_source.h"
#include "market/quote_publisher.h"
#include "strategies/quote_strategy.h"
#include "yquant/container/container.h"
#include "yquant/market/local_market_client.h"
#include "yquant/market/market_service.h"
#include "yquant/model/quote.h"
#include "yquant/strategy/strategy_service.h"

namespace yquant {
namespace {

// Stores poll order and thread identities in preallocated test storage.
struct PollTrace {
  std::array<int, 8> ids{};
  std::array<std::thread::id, 8> threads{};
  std::size_t count = 0;
};

// Records each poll and finishes after the specified positive number of polls.
// Keep the shared trace alive while using this component.
class CountPolls final : public container::Pollable {
 public:
  CountPolls(int id, int remaining, PollTrace& trace)
      : id_(id), remaining_(remaining), trace_(trace) {}
  CountPolls(const CountPolls&) = delete;
  CountPolls& operator=(const CountPolls&) = delete;

  container::PollResult Poll() override {
    EXPECT_LT(trace_.count, trace_.ids.size());
    if (trace_.count >= trace_.ids.size()) {
      return container::PollResult::kFinished;
    }
    trace_.ids[trace_.count] = id_;
    trace_.threads[trace_.count++] = std::this_thread::get_id();
    return --remaining_ == 0 ? container::PollResult::kFinished
                             : container::PollResult::kContinue;
  }

 private:
  int id_;
  int remaining_;
  PollTrace& trace_;
};

// Checks that callbacks cannot register components or start a nested Run.
class TryReentry final : public container::Pollable {
 public:
  TryReentry(container::Container& owner, container::Pollable& peer)
      : owner_(owner), peer_(peer) {}
  TryReentry(const TryReentry&) = delete;
  TryReentry& operator=(const TryReentry&) = delete;

  container::PollResult Poll() override {
    EXPECT_FALSE(owner_.Register(peer_));
    EXPECT_FALSE(owner_.Run());
    return container::PollResult::kFinished;
  }

 private:
  container::Container& owner_;
  container::Pollable& peer_;
};

// Saves every intent and its callback thread for comparison after Run returns.
class RecordIntents final : public examples::QuoteIntentListener {
 public:
  void OnQuoteIntent(const examples::QuoteIntent& intent) override {
    ASSERT_LT(count, intents.size());
    intents[count] = intent;
    threads[count++] = std::this_thread::get_id();
  }

  std::array<examples::QuoteIntent, 3> intents;
  std::array<std::thread::id, 3> threads{};
  std::size_t count = 0;
};

// Checks the polling thread and counts calls to the quote publisher.
class TracePublisher final : public container::Pollable {
 public:
  explicit TracePublisher(examples::QuotePublisher& publisher)
      : publisher_(publisher) {}
  TracePublisher(const TracePublisher&) = delete;
  TracePublisher& operator=(const TracePublisher&) = delete;

  container::PollResult Poll() override {
    EXPECT_EQ(std::this_thread::get_id(), caller);
    ++count;
    return publisher_.Poll();
  }

  std::thread::id caller = std::this_thread::get_id();
  std::size_t count = 0;

 private:
  examples::QuotePublisher& publisher_;
};

TEST(ContainerTest, PollsInRoundsAndSkipsFinishedComponents) {
  PollTrace trace;
  CountPolls first(1, 3, trace);
  CountPolls second(2, 1, trace);
  CountPolls third(3, 2, trace);
  container::Container runner;
  ASSERT_TRUE(runner.Register(first));
  ASSERT_TRUE(runner.Register(second));
  ASSERT_TRUE(runner.Register(third));
  ASSERT_TRUE(runner.Run());

  const std::array<int, 6> expected{1, 2, 3, 1, 3, 1};
  ASSERT_EQ(trace.count, expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(trace.ids[i], expected[i]);
    EXPECT_EQ(trace.threads[i], std::this_thread::get_id());
  }
}

TEST(ContainerTest, GrowsRegistrationsAndClearsThemAfterRun) {
  PollTrace trace;
  CountPolls first(1, 1, trace);
  CountPolls second(2, 1, trace);
  CountPolls third(3, 1, trace);
  CountPolls fourth(4, 1, trace);
  container::Container runner;
  EXPECT_TRUE(runner.Run());
  ASSERT_TRUE(runner.Register(first));
  EXPECT_FALSE(runner.Register(first));
  ASSERT_TRUE(runner.Register(second));
  ASSERT_TRUE(runner.Register(third));
  ASSERT_TRUE(runner.Run());
  EXPECT_EQ(trace.count, 3U);
  EXPECT_EQ(trace.ids[2], 3);
  ASSERT_TRUE(runner.Register(fourth));
  ASSERT_TRUE(runner.Run());
  EXPECT_EQ(trace.count, 4U);
  EXPECT_EQ(trace.ids[3], 4);
  EXPECT_TRUE(runner.Run());
  EXPECT_EQ(trace.count, 4U);
}

TEST(ContainerTest, RejectsRegistrationAndNestedRunDuringPolling) {
  PollTrace trace;
  CountPolls peer(1, 1, trace);
  container::Container runner;
  TryReentry component(runner, peer);
  ASSERT_TRUE(runner.Register(component));
  ASSERT_TRUE(runner.Run());
  EXPECT_EQ(trace.count, 0U);
  ASSERT_TRUE(runner.Register(peer));
  EXPECT_TRUE(runner.Run());
  EXPECT_EQ(trace.count, 1U);
}

TEST(ContainerTest, PublishesFiniteInputsBeforeRunReturns) {
  const std::array<model::Quote, 3> input{{
      {17, 201.5, 202.25, 13, 29, 7},
      {81, 98.75, 99.25, 5, 8, 12},
      {81, 98.75, 99.25, 21, 3, 31},
  }};
  for (std::size_t length : std::array<std::size_t, 3>{0, 1, 3}) {
    SCOPED_TRACE(length);
    RecordIntents output;
    examples::QuoteStrategy strategy(output);
    strategy::StrategyService strategy_service(strategy);
    market::MarketService market;
    market::LocalMarketClient client(market);
    client.Subscribe(17, strategy_service);
    client.Subscribe(81, strategy_service);
    examples::FixedQuoteSource source({input.data(), length});
    examples::QuotePublisher publisher(source, market);
    TracePublisher traced(publisher);
    container::Container runner;
    ASSERT_TRUE(runner.Register(traced));
    ASSERT_TRUE(runner.Run());

    EXPECT_TRUE(source.exhausted());
    EXPECT_EQ(traced.count, length == 0 ? 1 : length);
    ASSERT_EQ(output.count, length);
    for (std::size_t i = 0; i < length; ++i) {
      EXPECT_EQ(output.intents[i].sequence, input[i].sequence);
      EXPECT_EQ(output.intents[i].instrument_id, input[i].instrument_id);
      EXPECT_DOUBLE_EQ(output.intents[i].bid_price, input[i].bid_price);
      EXPECT_DOUBLE_EQ(output.intents[i].ask_price, input[i].ask_price);
      EXPECT_EQ(output.threads[i], std::this_thread::get_id());
    }
    ASSERT_TRUE(runner.Register(traced));
    EXPECT_TRUE(runner.Run());
    EXPECT_EQ(output.count, length);
    client.Unsubscribe(17, strategy_service);
    client.Unsubscribe(81, strategy_service);
  }
}

}  // namespace
}  // namespace yquant
