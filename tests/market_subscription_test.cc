#include <array>
#include <cstddef>

#include "gtest/gtest.h"

#include "yquant/market/local_market_client.h"
#include "yquant/market/market_client.h"
#include "yquant/market/market_service.h"
#include "yquant/model/quote.h"

namespace yquant::market {
namespace {

// Saves received quotes so tests can check delivery order and retained values.
class RecordQuotes final : public QuoteListener {
 public:
  void OnQuote(const model::Quote& quote) override {
    ASSERT_LT(count, quotes.size());
    quotes[count++] = quote;
  }

  std::array<model::Quote, 8> quotes;
  std::size_t count = 0;
};

TEST(MarketSubscriptionTest, FiltersInterleavedInstrumentsAndKeepsSnapshots) {
  MarketService service;
  LocalMarketClient local_client(service);
  MarketClient& client = local_client;
  RecordQuotes listener;
  client.Subscribe(17, listener);
  model::Quote quote{17, 201.5, 202.25, 13, 29, 1};
  service.Publish(quote);
  quote = {81, 98.75, 99.25, 5, 8, 2};
  service.Publish(quote);
  quote = {17, 202, 202.5, 7, 9, 3};
  service.Publish(quote);
  quote = {};

  ASSERT_EQ(listener.count, 2U);
  EXPECT_EQ(listener.quotes[0].instrument_id, 17U);
  EXPECT_EQ(listener.quotes[0].sequence, 1U);
  EXPECT_DOUBLE_EQ(listener.quotes[0].bid_price, 201.5);
  EXPECT_DOUBLE_EQ(listener.quotes[0].ask_price, 202.25);
  EXPECT_EQ(listener.quotes[0].bid_quantity, 13);
  EXPECT_EQ(listener.quotes[0].ask_quantity, 29);
  EXPECT_EQ(listener.quotes[1].instrument_id, 17U);
  EXPECT_EQ(listener.quotes[1].sequence, 3U);
  EXPECT_DOUBLE_EQ(listener.quotes[1].bid_price, 202);
  EXPECT_DOUBLE_EQ(listener.quotes[1].ask_price, 202.5);
  EXPECT_EQ(listener.quotes[1].bid_quantity, 7);
  EXPECT_EQ(listener.quotes[1].ask_quantity, 9);

  client.Unsubscribe(17, listener);
  service.Publish({17, 203, 204, 2, 4, 4});
  EXPECT_EQ(listener.count, 2U);
}

TEST(MarketSubscriptionTest, RepeatedOperationsAreIdempotent) {
  MarketService service;
  LocalMarketClient client(service);
  RecordQuotes listener;
  const model::Quote quote{17, 201.5, 202.25, 13, 29, 1};
  service.Publish(quote);
  client.Unsubscribe(17, listener);
  EXPECT_EQ(listener.count, 0U);

  client.Subscribe(17, listener);
  client.Subscribe(17, listener);
  service.Publish(quote);
  EXPECT_EQ(listener.count, 1U);
  client.Unsubscribe(17, listener);
  client.Unsubscribe(17, listener);
  service.Publish(quote);
  EXPECT_EQ(listener.count, 1U);

  client.Subscribe(17, listener);
  service.Publish(quote);
  EXPECT_EQ(listener.count, 2U);
  client.Unsubscribe(17, listener);
}

TEST(MarketSubscriptionTest, RemovingOneInstrumentKeepsOtherSubscriptions) {
  MarketService service;
  LocalMarketClient client(service);
  RecordQuotes listener;
  client.Subscribe(17, listener);
  client.Subscribe(81, listener);
  service.Publish({17, 201.5, 202.25, 13, 29, 1});
  service.Publish({81, 98.75, 99.25, 5, 8, 2});
  client.Unsubscribe(17, listener);
  service.Publish({17, 202, 202.5, 7, 9, 3});
  service.Publish({81, 99, 99.5, 6, 9, 4});

  ASSERT_EQ(listener.count, 3U);
  EXPECT_EQ(listener.quotes[0].sequence, 1U);
  EXPECT_EQ(listener.quotes[1].sequence, 2U);
  EXPECT_EQ(listener.quotes[2].instrument_id, 81U);
  EXPECT_EQ(listener.quotes[2].sequence, 4U);
  client.Unsubscribe(81, listener);
  service.Publish({81, 99, 99.5, 6, 9, 5});
  EXPECT_EQ(listener.count, 3U);
}

TEST(MarketSubscriptionTest, RemovingOneListenerKeepsOtherListeners) {
  MarketService service;
  LocalMarketClient first_client(service);
  LocalMarketClient second_client(service);
  RecordQuotes first;
  RecordQuotes second;
  const model::Quote quote{17, 201.5, 202.25, 13, 29, 1};
  first_client.Subscribe(17, first);
  second_client.Subscribe(17, second);
  service.Publish(quote);
  EXPECT_EQ(first.count, 1U);
  EXPECT_EQ(second.count, 1U);

  first_client.Unsubscribe(17, first);
  first_client.Unsubscribe(81, second);
  service.Publish(quote);
  EXPECT_EQ(first.count, 1U);
  EXPECT_EQ(second.count, 2U);
  first_client.Subscribe(17, first);
  service.Publish(quote);
  EXPECT_EQ(first.count, 2U);
  EXPECT_EQ(second.count, 3U);
  first_client.Unsubscribe(17, first);
  second_client.Unsubscribe(17, second);
}

}  // namespace
}  // namespace yquant::market
