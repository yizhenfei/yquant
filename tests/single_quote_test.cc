#include <thread>

#include "gtest/gtest.h"

#include "quote_source_stub.h"
#include "strategies/quote_strategy.h"
#include "yquant/market/local_market_client.h"
#include "yquant/market/market_client.h"
#include "yquant/market/market_service.h"
#include "yquant/model/quote.h"
#include "yquant/strategy/strategy_service.h"

namespace yquant {
namespace {

// Captures payload, address and thread to check synchronous quote delivery.
class RecordQuote final : public market::QuoteListener {
 public:
  // market::QuoteListener implementation.
  void OnQuote(const model::Quote& value) override {
    quote = value;
    address = &value;
    thread = std::this_thread::get_id();
    ++count;
  }

  model::Quote quote;
  const model::Quote* address = nullptr;
  std::thread::id thread;
  int count = 0;
};

// Captures emitted intents and their delivery context for strategy assertions.
class RecordIntent final : public examples::QuoteIntentListener {
 public:
  // examples::QuoteIntentListener implementation.
  void OnQuoteIntent(const examples::QuoteIntent& value) override {
    intent = value;
    thread = std::this_thread::get_id();
    ++count;
  }

  examples::QuoteIntent intent;
  std::thread::id thread;
  int count = 0;
};

TEST(SingleQuoteTest, ClientDeliversUnchangedQuoteWithoutCopyOnCallerThread) {
  test::QuoteSourceStub source;
  source.SetQuote({17, 203.5, 204.25, 13, 29});
  market::MarketService service(source);
  market::LocalMarketClient local_client(service);
  market::MarketClient& client = local_client;
  RecordQuote listener;
  client.Subscribe(17, listener);
  service.Run();

  EXPECT_EQ(listener.count, 1);
  EXPECT_EQ(listener.address, source.last_output);
  EXPECT_EQ(listener.thread, std::this_thread::get_id());
  EXPECT_EQ(listener.quote.instrument_id, 17U);
  EXPECT_DOUBLE_EQ(listener.quote.bid_price, 203.5);
  EXPECT_DOUBLE_EQ(listener.quote.ask_price, 204.25);
  EXPECT_EQ(listener.quote.bid_quantity, 13);
  EXPECT_EQ(listener.quote.ask_quantity, 29);
  client.Unsubscribe(17, listener);
}

TEST(SingleQuoteTest, StrategyProducesOneIntentFromReceivedPrices) {
  RecordIntent output;
  test::QuoteSourceStub source;
  source.SetQuote({81, 98.75, 99.25, 5, 8});
  market::MarketService service(source);
  market::LocalMarketClient client(service);
  examples::QuoteStrategy strategy(output);
  strategy::StrategyService strategy_service(strategy);
  client.Subscribe(81, strategy_service);
  service.Run();

  EXPECT_EQ(output.count, 1);
  EXPECT_EQ(output.thread, std::this_thread::get_id());
  EXPECT_EQ(output.intent.instrument_id, 81U);
  EXPECT_DOUBLE_EQ(output.intent.bid_price, 98.75);
  EXPECT_DOUBLE_EQ(output.intent.ask_price, 99.25);
  client.Unsubscribe(81, strategy_service);
}

}  // namespace
}  // namespace yquant
