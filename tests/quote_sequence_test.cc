#include <array>
#include <cstddef>

#include "gtest/gtest.h"

#include "market/fixed_quote_source.h"
#include "strategies/quote_strategy.h"
#include "yquant/market/local_market_client.h"
#include "yquant/market/market_client.h"
#include "yquant/market/market_service.h"
#include "yquant/market/quote_source.h"
#include "yquant/model/quote.h"
#include "yquant/strategy/strategy_service.h"

namespace yquant {
namespace {

// Saves a copy of each intent for comparison after the sequence ends.
class RecordIntents final : public examples::QuoteIntentListener {
 public:
  // examples::QuoteIntentListener implementation.
  void OnQuoteIntent(const examples::QuoteIntent& intent) override {
    ASSERT_LT(count, intents.size());
    intents[count++] = intent;
  }

  std::array<examples::QuoteIntent, 3> intents;
  std::size_t count = 0;
};

// Saves each quote and its address, then sends the quote to downstream.
// Keep the downstream listener alive until this recorder is destroyed.
class RecordQuotes final : public market::QuoteListener {
 public:
  explicit RecordQuotes(market::QuoteListener& downstream)
      : downstream_(downstream) {}
  RecordQuotes(const RecordQuotes&) = delete;
  RecordQuotes& operator=(const RecordQuotes&) = delete;

  // market::QuoteListener implementation.
  void OnQuote(const model::Quote& quote) override {
    ASSERT_LT(count, quotes.size());
    quotes[count] = quote;
    addresses[count++] = &quote;
    downstream_.OnQuote(quote);
  }

  std::array<model::Quote, 3> quotes;
  std::array<const model::Quote*, 3> addresses{};
  std::size_t count = 0;

 private:
  market::QuoteListener& downstream_;
};

// Checks that every field in actual matches the corresponding expected field.
void ExpectQuote(const model::Quote& actual, const model::Quote& expected) {
  EXPECT_EQ(actual.instrument_id, expected.instrument_id);
  EXPECT_DOUBLE_EQ(actual.bid_price, expected.bid_price);
  EXPECT_DOUBLE_EQ(actual.ask_price, expected.ask_price);
  EXPECT_EQ(actual.bid_quantity, expected.bid_quantity);
  EXPECT_EQ(actual.ask_quantity, expected.ask_quantity);
  EXPECT_EQ(actual.sequence, expected.sequence);
}

TEST(QuoteSequenceTest, RetainsSnapshotsAndIntentsAcrossBufferReuse) {
  const std::array<model::Quote, 3> input{{
      {17, 201.5, 202.25, 13, 29, 7},
      {81, 98.75, 99.25, 5, 8, 12},
      {81, 98.75, 99.25, 21, 3, 31},
  }};
  for (int run = 0; run < 2; ++run) {
    RecordIntents output;
    examples::QuoteStrategy strategy(output);
    strategy::StrategyService strategy_service(strategy);
    RecordQuotes receiver(strategy_service);
    examples::FixedQuoteSource source(input);
    market::MarketService market(source);
    market::LocalMarketClient client(market);
    client.Subscribe(17, receiver);
    client.Subscribe(81, receiver);
    EXPECT_FALSE(source.exhausted());
    market.Run();
    EXPECT_EQ(receiver.count, input.size());

    EXPECT_TRUE(source.exhausted());
    market.Run();
    market.Run();
    ASSERT_EQ(receiver.count, input.size());
    ASSERT_EQ(output.count, input.size());
    ASSERT_NE(receiver.addresses[0], nullptr);
    for (std::size_t i = 0; i < input.size(); ++i) {
      SCOPED_TRACE(i);
      ExpectQuote(receiver.quotes[i], input[i]);
      EXPECT_EQ(receiver.addresses[i], receiver.addresses[0]);
      EXPECT_EQ(output.intents[i].instrument_id, input[i].instrument_id);
      EXPECT_EQ(output.intents[i].sequence, input[i].sequence);
      EXPECT_DOUBLE_EQ(output.intents[i].bid_price, input[i].bid_price);
      EXPECT_DOUBLE_EQ(output.intents[i].ask_price, input[i].ask_price);
    }
    client.Unsubscribe(17, receiver);
    client.Unsubscribe(81, receiver);
  }
}

TEST(QuoteSequenceTest, EmptyInputLeavesOutputUnchanged) {
  examples::FixedQuoteSource source({});
  const model::Quote sentinel{9, 7.5, 8.5, 2, 3, 99};
  model::Quote quote = sentinel;
  EXPECT_TRUE(source.exhausted());
  EXPECT_EQ(source.Read(quote), market::QuoteReadResult::kIdle);
  ExpectQuote(quote, sentinel);
}

TEST(QuoteSequenceTest, DeliversLastQuoteOnceAndPreservesItWhileIdle) {
  const std::array<model::Quote, 1> input{{{3, 10.5, 11.25, 6, 4, 23}}};
  examples::FixedQuoteSource source(input);
  model::Quote quote;
  EXPECT_FALSE(source.exhausted());
  ASSERT_EQ(source.Read(quote), market::QuoteReadResult::kQuote);
  EXPECT_TRUE(source.exhausted());
  ExpectQuote(quote, input.front());
  EXPECT_EQ(source.Read(quote), market::QuoteReadResult::kIdle);
  EXPECT_EQ(source.Read(quote), market::QuoteReadResult::kIdle);
  ExpectQuote(quote, input.front());
}

}  // namespace
}  // namespace yquant
