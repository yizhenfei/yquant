#include "yquant/market/market_client_factory.h"

#include <memory>
#include <thread>

#include "gtest/gtest.h"

#include "quote_source_stub.h"
#include "yquant/market/market_client.h"
#include "yquant/market/market_service.h"
#include "yquant/market/market_service_registry.h"
#include "yquant/model/quote.h"

namespace yquant::market {
namespace {

// Copies each callback's quote and checks that the service delivered the source
// buffer directly. Keep source alive while using this listener.
class RecordQuote final : public QuoteListener {
 public:
  explicit RecordQuote(const test::QuoteSourceStub& source) : source_(source) {}
  RecordQuote(const RecordQuote&) = delete;
  RecordQuote& operator=(const RecordQuote&) = delete;

  void OnQuote(const model::Quote& value) override {
    EXPECT_EQ(&value, source_.last_output);
    quote = value;
    thread = std::this_thread::get_id();
    ++count;
  }

  model::Quote quote;
  std::thread::id thread;
  int count = 0;

 private:
  const test::QuoteSourceStub& source_;
};

// Checks every field of a retained quote against the injected input.
void ExpectQuote(const model::Quote& actual, const model::Quote& expected) {
  EXPECT_EQ(actual.instrument_id, expected.instrument_id);
  EXPECT_DOUBLE_EQ(actual.bid_price, expected.bid_price);
  EXPECT_DOUBLE_EQ(actual.ask_price, expected.ask_price);
  EXPECT_EQ(actual.bid_quantity, expected.bid_quantity);
  EXPECT_EQ(actual.ask_quantity, expected.ask_quantity);
  EXPECT_EQ(actual.sequence, expected.sequence);
}

TEST(MarketClientFactoryTest, RoutesIdsToTheirRegisteredServices) {
  test::QuoteSourceStub first_source;
  test::QuoteSourceStub second_source;
  MarketService first_service(first_source);
  MarketService second_service(second_source);
  MarketServiceRegistry local_services;
  MarketClientFactory factory(local_services);
  ASSERT_TRUE(local_services.Register(11, first_service));
  ASSERT_TRUE(local_services.Register(22, second_service));
  auto first_client = factory.Create(11);
  auto second_client = factory.Create(22);
  ASSERT_NE(first_client, nullptr);
  ASSERT_NE(second_client, nullptr);
  RecordQuote first(first_source);
  RecordQuote second(second_source);
  first_client->Subscribe(17, first);
  second_client->Subscribe(17, second);

  const model::Quote first_quote{17, 201.5, 202.25, 13, 29, 7};
  const model::Quote second_quote{17, 98.75, 99.25, 5, 8, 12};
  first_source.SetQuote(first_quote);
  second_source.SetQuote(second_quote);
  first_service.Run();
  EXPECT_EQ(first.count, 1);
  EXPECT_EQ(second.count, 0);
  ExpectQuote(first.quote, first_quote);
  EXPECT_EQ(first.thread, std::this_thread::get_id());
  second_service.Run();
  EXPECT_EQ(first.count, 1);
  EXPECT_EQ(second.count, 1);
  ExpectQuote(second.quote, second_quote);
  EXPECT_EQ(second.thread, std::this_thread::get_id());

  first_client->Unsubscribe(17, first);
  second_client->Unsubscribe(17, second);
  first_source.SetQuote(first_quote);
  second_source.SetQuote(second_quote);
  first_service.Run();
  second_service.Run();
  EXPECT_EQ(first.count, 1);
  EXPECT_EQ(second.count, 1);
}

TEST(MarketClientFactoryTest, FactoriesUseTheirInjectedRegistries) {
  test::QuoteSourceStub first_source;
  test::QuoteSourceStub second_source;
  MarketService first_service(first_source);
  MarketService second_service(second_source);
  MarketServiceRegistry first_registry;
  MarketServiceRegistry second_registry;
  ASSERT_TRUE(first_registry.Register(11, first_service));
  ASSERT_TRUE(second_registry.Register(11, second_service));
  MarketClientFactory first_factory(first_registry);
  MarketClientFactory second_factory(second_registry);
  auto first_client = first_factory.Create(11);
  auto second_client = second_factory.Create(11);
  ASSERT_NE(first_client, nullptr);
  ASSERT_NE(second_client, nullptr);
  RecordQuote first(first_source);
  RecordQuote second(second_source);
  first_client->Subscribe(17, first);
  second_client->Subscribe(17, second);

  first_source.SetQuote({17, 201.5, 202.25, 13, 29, 7});
  first_service.Run();
  EXPECT_EQ(first.count, 1);
  EXPECT_EQ(second.count, 0);
  second_source.SetQuote({17, 98.75, 99.25, 5, 8, 12});
  second_service.Run();
  EXPECT_EQ(first.count, 1);
  EXPECT_EQ(second.count, 1);
  EXPECT_EQ(first.quote.sequence, 7U);
  EXPECT_EQ(second.quote.sequence, 12U);
  first_client->Unsubscribe(17, first);
  second_client->Unsubscribe(17, second);
}

TEST(MarketClientFactoryTest, DuplicateIdPreservesTheOriginalBinding) {
  test::QuoteSourceStub original_source;
  test::QuoteSourceStub replacement_source;
  MarketService original(original_source);
  MarketService replacement(replacement_source);
  MarketServiceRegistry local_services;
  MarketClientFactory factory(local_services);
  ASSERT_TRUE(local_services.Register(7, original));
  EXPECT_FALSE(local_services.Register(7, original));
  EXPECT_FALSE(local_services.Register(7, replacement));
  auto client = factory.Create(7);
  ASSERT_NE(client, nullptr);
  RecordQuote listener(original_source);
  client->Subscribe(17, listener);

  const model::Quote quote{17, 201.5, 202.25, 13, 29, 7};
  replacement_source.SetQuote(quote);
  replacement.Run();
  EXPECT_EQ(listener.count, 0);
  original_source.SetQuote(quote);
  original.Run();
  EXPECT_EQ(listener.count, 1);
  ExpectQuote(listener.quote, quote);
  client->Unsubscribe(17, listener);
}

TEST(MarketClientFactoryTest, MissingIdReturnsNullAndCanBeRegisteredLater) {
  test::QuoteSourceStub existing_source;
  test::QuoteSourceStub source;
  MarketService existing_service(existing_source);
  MarketService service(source);
  MarketServiceRegistry local_services;
  MarketClientFactory factory(local_services);
  EXPECT_EQ(factory.Create(77), nullptr);
  ASSERT_TRUE(local_services.Register(11, existing_service));
  EXPECT_EQ(factory.Create(77), nullptr);
  ASSERT_TRUE(local_services.Register(77, service));
  auto client = factory.Create(77);
  ASSERT_NE(client, nullptr);
  RecordQuote listener(source);
  client->Subscribe(17, listener);
  const model::Quote quote{17, 201.5, 202.25, 13, 29, 7};
  source.SetQuote(quote);
  service.Run();
  EXPECT_EQ(listener.count, 1);
  ExpectQuote(listener.quote, quote);
  client->Unsubscribe(17, listener);
}

TEST(MarketClientFactoryTest,
     ClientRemainsUsableAfterFactoryAndRegistryDestruction) {
  test::QuoteSourceStub source;
  MarketService service(source);
  std::unique_ptr<MarketClient> client;
  {
    MarketServiceRegistry local_services;
    MarketClientFactory factory(local_services);
    ASSERT_TRUE(local_services.Register(31, service));
    client = factory.Create(31);
  }
  ASSERT_NE(client, nullptr);
  RecordQuote listener(source);
  client->Subscribe(17, listener);
  const model::Quote quote{17, 201.5, 202.25, 13, 29, 7};
  source.SetQuote(quote);
  service.Run();
  EXPECT_EQ(listener.count, 1);
  ExpectQuote(listener.quote, quote);
  client->Unsubscribe(17, listener);
}

TEST(MarketClientFactoryTest, ClientsForSameIdSupportDifferentListeners) {
  test::QuoteSourceStub source;
  MarketService service(source);
  MarketServiceRegistry local_services;
  MarketClientFactory factory(local_services);
  ASSERT_TRUE(local_services.Register(5, service));
  auto first_client = factory.Create(5);
  auto second_client = factory.Create(5);
  ASSERT_NE(first_client, nullptr);
  ASSERT_NE(second_client, nullptr);
  RecordQuote first(source);
  RecordQuote second(source);
  first_client->Subscribe(17, first);
  second_client->Subscribe(17, second);
  const model::Quote quote{17, 201.5, 202.25, 13, 29, 7};
  source.SetQuote(quote);
  service.Run();
  EXPECT_EQ(first.count, 1);
  EXPECT_EQ(second.count, 1);

  first_client->Unsubscribe(17, first);
  first_client.reset();
  source.SetQuote(quote);
  service.Run();
  EXPECT_EQ(first.count, 1);
  EXPECT_EQ(second.count, 2);
  second_client->Unsubscribe(17, second);
}

}  // namespace
}  // namespace yquant::market
