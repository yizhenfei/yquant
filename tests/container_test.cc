#include <array>
#include <cstddef>
#include <thread>

#include "gtest/gtest.h"

#include "market/fixed_quote_source.h"
#include "strategies/quote_strategy.h"
#include "yquant/container/container.h"
#include "yquant/container/service.h"
#include "yquant/market/local_market_client.h"
#include "yquant/market/market_service.h"
#include "yquant/market/quote_source.h"
#include "yquant/model/quote.h"
#include "yquant/strategy/strategy_service.h"

namespace yquant {
namespace {

// Stores service call order and thread identities in preallocated storage.
struct RunTrace {
  std::array<int, 8> ids{};
  std::array<std::thread::id, 8> threads{};
  std::size_t count = 0;
};

// Records each Run call. Keep trace alive while using this service.
class RecordRuns final : public container::Service {
 public:
  RecordRuns(int id, RunTrace& trace) : id_(id), trace_(trace) {}
  RecordRuns(const RecordRuns&) = delete;
  RecordRuns& operator=(const RecordRuns&) = delete;

  // container::Service implementation.
  void Run() override {
    ASSERT_LT(trace_.count, trace_.ids.size());
    trace_.ids[trace_.count] = id_;
    trace_.threads[trace_.count++] = std::this_thread::get_id();
  }

 private:
  int id_;
  RunTrace& trace_;
};

// Checks that a running service cannot register services or start a nested Run.
class TryReentry final : public container::Service {
 public:
  TryReentry(container::Container& owner, container::Service& peer)
      : owner_(owner), peer_(peer) {}
  TryReentry(const TryReentry&) = delete;
  TryReentry& operator=(const TryReentry&) = delete;

  // container::Service implementation.
  void Run() override {
    EXPECT_FALSE(owner_.Register(peer_));
    EXPECT_FALSE(owner_.Run());
    ++runs;
  }

  int runs = 0;

 private:
  container::Container& owner_;
  container::Service& peer_;
};

// Saves every intent and its callback thread for comparison after Run returns.
class RecordIntents final : public examples::QuoteIntentListener {
 public:
  // examples::QuoteIntentListener implementation.
  void OnQuoteIntent(const examples::QuoteIntent& intent) override {
    ASSERT_LT(count, intents.size());
    intents[count] = intent;
    threads[count++] = std::this_thread::get_id();
  }

  std::array<examples::QuoteIntent, 3> intents;
  std::array<std::thread::id, 3> threads{};
  std::size_t count = 0;
};

// Checks the reading thread and counts calls to the injected quote source.
// Keep source alive until this wrapper is destroyed.
class TraceSource final : public market::QuoteSource {
 public:
  explicit TraceSource(market::QuoteSource& source) : source_(source) {}
  TraceSource(const TraceSource&) = delete;
  TraceSource& operator=(const TraceSource&) = delete;

  // market::QuoteSource implementation.
  market::QuoteReadResult Read(model::Quote& quote) override {
    EXPECT_EQ(std::this_thread::get_id(), caller);
    ++count;
    return source_.Read(quote);
  }

  std::thread::id caller = std::this_thread::get_id();
  std::size_t count = 0;

 private:
  market::QuoteSource& source_;
};

TEST(ContainerTest, CallsEveryServiceInRegistrationOrderEachRound) {
  RunTrace trace;
  RecordRuns first(1, trace);
  RecordRuns second(2, trace);
  RecordRuns third(3, trace);
  container::Container runner;
  ASSERT_TRUE(runner.Register(first));
  ASSERT_TRUE(runner.Register(second));
  ASSERT_TRUE(runner.Register(third));
  ASSERT_TRUE(runner.Run([&trace] { return trace.count == 6; }));

  const std::array<int, 6> expected{1, 2, 3, 1, 2, 3};
  ASSERT_EQ(trace.count, expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(trace.ids[i], expected[i]);
    EXPECT_EQ(trace.threads[i], std::this_thread::get_id());
  }
}

TEST(ContainerTest, GrowsRegistrationsAndClearsThemAfterRun) {
  RunTrace trace;
  RecordRuns first(1, trace);
  RecordRuns second(2, trace);
  RecordRuns third(3, trace);
  RecordRuns fourth(4, trace);
  container::Container runner;
  EXPECT_TRUE(runner.Run());
  ASSERT_TRUE(runner.Register(first));
  EXPECT_FALSE(runner.Register(first));
  ASSERT_TRUE(runner.Register(second));
  ASSERT_TRUE(runner.Register(third));
  ASSERT_TRUE(runner.Run([&trace] { return trace.count == 3; }));
  EXPECT_EQ(trace.count, 3U);
  EXPECT_EQ(trace.ids[2], 3);
  ASSERT_TRUE(runner.Register(fourth));
  ASSERT_TRUE(runner.Run([&trace] { return trace.count == 4; }));
  EXPECT_EQ(trace.count, 4U);
  EXPECT_EQ(trace.ids[3], 4);
  EXPECT_TRUE(runner.Run());
  EXPECT_EQ(trace.count, 4U);
}

TEST(ContainerTest, RejectsRegistrationAndNestedContainerRunFromService) {
  RunTrace trace;
  RecordRuns peer(1, trace);
  container::Container runner;
  TryReentry service(runner, peer);
  ASSERT_TRUE(runner.Register(service));
  ASSERT_TRUE(runner.Run([&service] { return service.runs == 1; }));
  EXPECT_EQ(trace.count, 0U);
  ASSERT_TRUE(runner.Register(peer));
  EXPECT_TRUE(runner.Run([&trace] { return trace.count == 1; }));
  EXPECT_EQ(trace.count, 1U);
}

TEST(ContainerTest, ExitConditionSkipsFurtherCallsAndClearsRegistrations) {
  RunTrace trace;
  RecordRuns first(1, trace);
  RecordRuns later(2, trace);
  container::Container runner;
  ASSERT_TRUE(runner.Register(first));
  ASSERT_TRUE(runner.Register(later));
  ASSERT_TRUE(runner.Run([&trace] { return trace.count == 1; }));
  ASSERT_EQ(trace.count, 1U);
  EXPECT_EQ(trace.ids[0], 1);

  ASSERT_TRUE(runner.Run());
  EXPECT_EQ(trace.count, 1U);
  ASSERT_TRUE(runner.Register(first));
  ASSERT_TRUE(runner.Register(later));
  ASSERT_TRUE(runner.Run([&trace] { return trace.count == 3; }));
  ASSERT_EQ(trace.count, 3U);
  EXPECT_EQ(trace.ids[1], 1);
  EXPECT_EQ(trace.ids[2], 2);
}

TEST(ContainerTest, InitiallyTrueExitConditionPreventsAllServiceCalls) {
  RunTrace trace;
  RecordRuns service(1, trace);
  container::Container runner;
  ASSERT_TRUE(runner.Register(service));
  ASSERT_TRUE(runner.Run([] { return true; }));
  EXPECT_EQ(trace.count, 0U);
  ASSERT_TRUE(runner.Run());
  EXPECT_EQ(trace.count, 0U);
  ASSERT_TRUE(runner.Register(service));
  ASSERT_TRUE(runner.Run([&trace] { return trace.count == 1; }));
  EXPECT_EQ(trace.count, 1U);
}

TEST(ContainerTest, ExitConditionRunsOnCallingThreadAndRejectsReentry) {
  RunTrace trace;
  RecordRuns service(1, trace);
  RecordRuns peer(2, trace);
  container::Container runner;
  ASSERT_TRUE(runner.Register(service));
  const std::thread::id caller = std::this_thread::get_id();
  std::size_t checks = 0;
  ASSERT_TRUE(runner.Run([&] {
    EXPECT_EQ(std::this_thread::get_id(), caller);
    EXPECT_FALSE(runner.Register(peer));
    EXPECT_FALSE(runner.Run());
    EXPECT_FALSE(runner.Run([] { return false; }));
    ++checks;
    return trace.count == 1;
  }));
  EXPECT_EQ(checks, 2U);
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
    examples::FixedQuoteSource source({input.data(), length});
    TraceSource traced(source);
    market::MarketService market(traced);
    market::LocalMarketClient client(market);
    client.Subscribe(17, strategy_service);
    client.Subscribe(81, strategy_service);
    container::Container runner;
    ASSERT_TRUE(runner.Register(market));
    ASSERT_TRUE(runner.Run([&source] { return source.exhausted(); }));

    EXPECT_TRUE(source.exhausted());
    EXPECT_EQ(traced.count, length == 0 ? 0 : length + 1);
    ASSERT_EQ(output.count, length);
    for (std::size_t i = 0; i < length; ++i) {
      EXPECT_EQ(output.intents[i].sequence, input[i].sequence);
      EXPECT_EQ(output.intents[i].instrument_id, input[i].instrument_id);
      EXPECT_DOUBLE_EQ(output.intents[i].bid_price, input[i].bid_price);
      EXPECT_DOUBLE_EQ(output.intents[i].ask_price, input[i].ask_price);
      EXPECT_EQ(output.threads[i], std::this_thread::get_id());
    }
    ASSERT_TRUE(runner.Register(market));
    EXPECT_TRUE(runner.Run([&source] { return source.exhausted(); }));
    EXPECT_EQ(output.count, length);
    EXPECT_EQ(traced.count, length == 0 ? 0 : length + 1);
    client.Unsubscribe(17, strategy_service);
    client.Unsubscribe(81, strategy_service);
  }
}

}  // namespace
}  // namespace yquant
