#include <array>
#include <cstddef>
#include <span>
#include <thread>

#include "gtest/gtest.h"

#include "yquant/container/container.h"
#include "yquant/market/market_client.h"
#include "yquant/market/market_service.h"
#include "yquant/market/quote_source.h"
#include "yquant/model/quote.h"

namespace yquant::market {
namespace {

// Checks all fields so a changed or partially copied quote fails the test.
void ExpectQuote(const model::Quote& actual, const model::Quote& expected) {
  EXPECT_EQ(actual.instrument_id, expected.instrument_id);
  EXPECT_DOUBLE_EQ(actual.bid_price, expected.bid_price);
  EXPECT_DOUBLE_EQ(actual.ask_price, expected.ask_price);
  EXPECT_EQ(actual.bid_quantity, expected.bid_quantity);
  EXPECT_EQ(actual.ask_quantity, expected.ask_quantity);
  EXPECT_EQ(actual.sequence, expected.sequence);
}

// Describes one source read; quote is used only for kQuote results.
struct SourceEvent {
  QuoteReadResult result;
  model::Quote quote{};
};

// Returns each scripted result in order, then returns kIdle.
// Keep the event array alive while using this source.
// Set reenter_service to call Run once from the next Read.
class ScriptedSource final : public QuoteSource {
 public:
  explicit ScriptedSource(std::span<const SourceEvent> events)
      : events_(events) {}
  ScriptedSource(const ScriptedSource&) = default;
  ScriptedSource& operator=(const ScriptedSource&) = default;

  QuoteReadResult Read(model::Quote& quote) override {
    EXPECT_LT(read_count, addresses.size());
    if (read_count >= addresses.size()) {
      return QuoteReadResult::kIdle;
    }
    addresses[read_count] = &quote;
    threads[read_count++] = std::this_thread::get_id();
    if (reenter_service != nullptr) {
      MarketService* service = reenter_service;
      reenter_service = nullptr;
      service->Run();
    }
    if (next_ == events_.size()) {
      return QuoteReadResult::kIdle;
    }
    const SourceEvent& event = events_[next_++];
    if (event.result == QuoteReadResult::kQuote) {
      quote = event.quote;
    }
    return event.result;
  }

  std::array<const model::Quote*, 256> addresses{};
  std::array<std::thread::id, 256> threads{};
  std::size_t read_count = 0;
  MarketService* reenter_service = nullptr;

 private:
  std::span<const SourceEvent> events_;
  std::size_t next_ = 0;
};

// Saves copies, addresses, and callback threads for later checks. When
// reenter_service is set, the next callback attempts a nested Run and checks
// that the callback's input remains unchanged until it returns.
class RecordQuotes final : public QuoteListener {
 public:
  void OnQuote(const model::Quote& quote) override {
    ASSERT_LT(count, quotes.size());
    const std::size_t index = count++;
    quotes[index] = quote;
    addresses[index] = &quote;
    threads[index] = std::this_thread::get_id();
    if (reenter_service != nullptr) {
      MarketService* service = reenter_service;
      reenter_service = nullptr;
      service->Run();
      ExpectQuote(quote, quotes[index]);
    }
  }

  std::array<model::Quote, 256> quotes;
  std::array<const model::Quote*, 256> addresses{};
  std::array<std::thread::id, 256> threads{};
  std::size_t count = 0;
  MarketService* reenter_service = nullptr;
};

TEST(MarketSourceTest, IdleReadsDoNotRedeliverThePreviousQuote) {
  const std::array<SourceEvent, 6> events{{
      {QuoteReadResult::kIdle},
      {QuoteReadResult::kQuote, {17, 201.5, 202.25, 13, 29, 7}},
      {QuoteReadResult::kIdle},
      {QuoteReadResult::kQuote, {17, 203, 204.5, 5, 8, 12}},
      {QuoteReadResult::kIdle},
      {QuoteReadResult::kIdle},
  }};
  ScriptedSource source(events);
  RecordQuotes listener;
  MarketService service(source);
  service.Subscribe(17, listener);

  const std::array<std::size_t, 4> expected_counts{0, 1, 2, 2};
  const std::array<std::size_t, 4> expected_reads{1, 3, 5, 6};
  for (std::size_t i = 0; i < expected_counts.size(); ++i) {
    SCOPED_TRACE(i);
    service.Run();
    EXPECT_EQ(source.read_count, expected_reads[i]);
    EXPECT_EQ(listener.count, expected_counts[i]);
  }
  EXPECT_EQ(source.read_count, events.size());
  ASSERT_EQ(listener.count, 2U);
  ExpectQuote(listener.quotes[0], events[1].quote);
  ExpectQuote(listener.quotes[1], events[3].quote);
}

TEST(MarketSourceTest, KeepsReadingWhenNoQuotesAreAvailable) {
  ScriptedSource source({});
  RecordQuotes listener;
  MarketService service(source);
  service.Subscribe(17, listener);

  service.Run();
  service.Run();
  EXPECT_EQ(source.read_count, 2U);
  EXPECT_EQ(listener.count, 0U);
}

TEST(MarketSourceTest, ResumesReadingQuotesAfterIdle) {
  const std::array<SourceEvent, 3> events{{
      {QuoteReadResult::kQuote, {17, 201.5, 202.25, 13, 29, 7}},
      {QuoteReadResult::kIdle},
      {QuoteReadResult::kQuote, {17, 203, 204.5, 5, 8, 12}},
  }};
  ScriptedSource source(events);
  RecordQuotes listener;
  MarketService service(source);
  service.Subscribe(17, listener);

  service.Run();
  ASSERT_EQ(listener.count, 1U);
  ExpectQuote(listener.quotes[0], events[0].quote);
  service.Run();
  EXPECT_EQ(source.read_count, 4U);
  ASSERT_EQ(listener.count, 2U);
  ExpectQuote(listener.quotes[1], events[2].quote);
}

TEST(MarketSourceTest, DrainsAvailableQuotesAndPreservesRetainedSnapshots) {
  const std::array<SourceEvent, 3> events{{
      {QuoteReadResult::kQuote, {17, 201.5, 202.25, 13, 29, 7}},
      {QuoteReadResult::kQuote, {81, 98.75, 99.25, 5, 8, 12}},
      {QuoteReadResult::kQuote, {81, 99, 99.5, 21, 3, 31}},
  }};
  ScriptedSource source(events);
  RecordQuotes listener;
  MarketService service(source);
  service.Subscribe(17, listener);
  service.Subscribe(81, listener);
  service.Run();

  EXPECT_EQ(source.read_count, events.size() + 1);
  ASSERT_EQ(listener.count, events.size());
  for (std::size_t i = 0; i < events.size(); ++i) {
    SCOPED_TRACE(i);
    ExpectQuote(listener.quotes[i], events[i].quote);
    EXPECT_EQ(source.addresses[i], source.addresses[0]);
    EXPECT_EQ(listener.addresses[i], source.addresses[0]);
  }
  service.Run();
  EXPECT_EQ(source.read_count, events.size() + 2);
  EXPECT_EQ(listener.count, events.size());
}

TEST(MarketSourceTest, BusySourceReturnsAtBatchLimitAndContinuesNextRun) {
  std::array<SourceEvent, 128> events{};
  for (std::size_t i = 0; i < events.size(); ++i) {
    events[i] = {QuoteReadResult::kQuote, {17, 201.5, 202.25, 13, 29, i + 1}};
  }
  ScriptedSource source(events);
  RecordQuotes listener;
  MarketService service(source);
  service.Subscribe(17, listener);

  service.Run();
  const std::size_t batch_size = listener.count;
  ASSERT_GT(batch_size, 1U);
  ASSERT_LT(batch_size, events.size());
  EXPECT_EQ(source.read_count, batch_size);

  while (listener.count < events.size()) {
    const std::size_t previous_count = listener.count;
    service.Run();
    ASSERT_GT(listener.count, previous_count);
    EXPECT_LE(listener.count - previous_count, batch_size);
  }
  ASSERT_EQ(listener.count, events.size());
  for (std::size_t i = 0; i < events.size(); ++i) {
    SCOPED_TRACE(i);
    ExpectQuote(listener.quotes[i], events[i].quote);
  }
  service.Run();
  EXPECT_EQ(listener.count, events.size());
}

TEST(MarketSourceTest, ReadsAndDeliversQuotesOnTheThreadCallingRun) {
  const std::array<SourceEvent, 2> events{{
      {QuoteReadResult::kIdle},
      {QuoteReadResult::kQuote, {17, 201.5, 202.25, 13, 29, 7}},
  }};
  ScriptedSource source(events);
  RecordQuotes listener;
  MarketService service(source);
  service.Subscribe(17, listener);
  container::Container runner;
  ASSERT_TRUE(runner.Register(service));
  const std::thread::id run_thread_id = std::this_thread::get_id();
  ASSERT_TRUE(runner.Run(
      [&source, &events] { return source.read_count >= events.size(); }));

  ASSERT_EQ(source.read_count, events.size() + 1);
  for (std::size_t i = 0; i < source.read_count; ++i) {
    EXPECT_EQ(source.threads[i], run_thread_id);
  }
  ASSERT_EQ(listener.count, 1U);
  EXPECT_EQ(listener.threads[0], run_thread_id);
}

TEST(MarketSourceTest, NestedRunFromListenerPreservesCurrentAndNextQuotes) {
  const std::array<SourceEvent, 2> events{{
      {QuoteReadResult::kQuote, {17, 201.5, 202.25, 13, 29, 7}},
      {QuoteReadResult::kQuote, {17, 203, 204.5, 5, 8, 12}},
  }};
  ScriptedSource source(events);
  RecordQuotes first;
  RecordQuotes second;
  MarketService service(source);
  first.reenter_service = &service;
  service.Subscribe(17, first);
  service.Subscribe(17, second);

  service.Run();
  EXPECT_EQ(source.read_count, events.size() + 1);
  ASSERT_EQ(first.count, events.size());
  ASSERT_EQ(second.count, events.size());
  for (std::size_t i = 0; i < events.size(); ++i) {
    ExpectQuote(first.quotes[i], events[i].quote);
    ExpectQuote(second.quotes[i], events[i].quote);
  }
  service.Run();
}

TEST(MarketSourceTest, NestedRunFromSourceDoesNotConsumeAnotherEvent) {
  const std::array<SourceEvent, 2> events{{
      {QuoteReadResult::kQuote, {17, 201.5, 202.25, 13, 29, 7}},
      {QuoteReadResult::kQuote, {17, 203, 204.5, 5, 8, 12}},
  }};
  ScriptedSource source(events);
  RecordQuotes listener;
  MarketService service(source);
  source.reenter_service = &service;
  service.Subscribe(17, listener);

  service.Run();
  EXPECT_EQ(source.read_count, events.size() + 1);
  ASSERT_EQ(listener.count, events.size());
  for (std::size_t i = 0; i < events.size(); ++i) {
    ExpectQuote(listener.quotes[i], events[i].quote);
  }
  service.Run();
}

}  // namespace
}  // namespace yquant::market
