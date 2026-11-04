#include "yquant/market/simnow_quote_source.h"

#include <thread>
#include <vector>

#include "fake_md_api.h"
#include "gtest/gtest.h"
#include "simnow_session.h"
#include "yquant/market/market_client_factory.h"
#include "yquant/market/market_service_registry.h"

namespace yquant::market {
namespace {

// Uses one SDK object and completes each requested subscription independently.
class MultiMarketTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config.front = "tcp://fixture:1";
    config.anonymous = true;
    config.subscriptions = {{"TFtest",42},{"Ttest",81},{"IFtest",99}};
    source = internal::StartSession(config, [&] { ++creates; return &api; });
    ASSERT_TRUE(source);
    EXPECT_FALSE(source->ready());
    api.spi->OnFrontConnected();
    source->TryRead(quote);
    api.LoginReply();
    source->TryRead(quote);
  }
  SimNowMarketConfig config;
  test::FakeMdApi api;
  std::unique_ptr<SimNowQuoteSource> source;
  model::Quote quote{};
  int creates = 0;
};

// Retains quote identities and execution threads for the public client path.
class Seen final : public QuoteListener {
 public:
  void OnQuote(const model::Quote& quote) override {
    quotes.push_back(quote);
    threads.push_back(std::this_thread::get_id());
  }
  std::vector<model::Quote> quotes;
  std::vector<std::thread::id> threads;
};

TEST_F(MultiMarketTest, EntireGroupMustConfirmBeforeBusinessThreadDelivery) {
  EXPECT_EQ(creates,1);
  EXPECT_EQ(api.login_calls,1);
  EXPECT_EQ(api.sub_calls,1);
  EXPECT_EQ(api.symbols,(std::vector<std::string>{"TFtest","Ttest","IFtest"}));
  config.subscriptions[0].symbol = "changed";  // Config storage is not retained.
  MarketService market(*source);
  MarketServiceRegistry registry;
  ASSERT_TRUE(registry.Register(1,market));
  auto client = MarketClientFactory(registry).Create(1);
  Seen first, same, second, unrelated;
  client->Subscribe(42,first);
  client->Subscribe(42,same);
  client->Subscribe(81,second);
  client->Subscribe(7,unrelated);
  std::thread([&] {
    api.SubReply("Ttest",0,false);
    api.Tick("Ttest");
    api.SubReply("TFtest",0,true);  // The final flag cannot account for missing IFtest.
    api.SubReply("TFtest");
  }).join();
  market.Run();
  EXPECT_FALSE(source->ready());
  EXPECT_TRUE(first.quotes.empty());
  EXPECT_TRUE(second.quotes.empty());
  std::thread([&] {
    api.SubReply("IFtest",0,true);
    api.Tick("unknown");
    api.Tick("Ttest",100,101,4);
    api.Tick("TFtest",102,103,6);
    api.Tick("Ttest",104,105,8);
    api.Tick("IFtest");
  }).join();
  EXPECT_TRUE(source->ready());
  market.Run();
  ASSERT_EQ(first.quotes.size(),1);
  ASSERT_EQ(same.quotes.size(),1);
  ASSERT_EQ(second.quotes.size(),2);
  EXPECT_TRUE(unrelated.quotes.empty());
  EXPECT_EQ(first.quotes[0].instrument_id,42);
  EXPECT_EQ(first.quotes[0].sequence,1);
  EXPECT_EQ(second.quotes[0].sequence,1);
  EXPECT_EQ(second.quotes[1].sequence,2);
  EXPECT_EQ(second.quotes[1].bid_quantity,8);
  EXPECT_EQ(first.threads[0],std::this_thread::get_id());
  EXPECT_EQ(second.threads[1],std::this_thread::get_id());
  client->Unsubscribe(42,first);
  client->Unsubscribe(42,same);
  client->Unsubscribe(81,second);
  client->Unsubscribe(7,unrelated);
}

TEST_F(MultiMarketTest, PartialFailureIsTerminalAndCannotExposeConfirmedContracts) {
  api.SubReply("TFtest",0,false);
  api.Tick("TFtest");
  api.SubReply("Ttest",7,true);
  api.SubReply("IFtest");
  api.Tick("TFtest");
  EXPECT_TRUE(source->failed());
  EXPECT_FALSE(source->ready());
  quote.instrument_id = 777;
  EXPECT_FALSE(source->TryRead(quote));
  EXPECT_EQ(quote.instrument_id,777);
}

TEST_F(MultiMarketTest, NullFinalMarkerWorksOnlyAfterAllContractsWereAcknowledged) {
  for (const auto& name : api.symbols) api.SubReply(name.c_str(),0,false);
  EXPECT_FALSE(source->ready());
  api.spi->OnRspSubMarketData(nullptr,nullptr,0,true);
  EXPECT_TRUE(source->ready());
  api.Tick("TFtest");
  api.spi->OnFrontDisconnected(4097);
  EXPECT_FALSE(source->ready());
  EXPECT_FALSE(source->TryRead(quote));
  EXPECT_TRUE(source->failed());
}

TEST_F(MultiMarketTest, UnknownAcknowledgmentFailsTheGroup) {
  api.SubReply("wrong");
  EXPECT_TRUE(source->failed());
  EXPECT_FALSE(source->ready());
}

TEST_F(MultiMarketTest, MissingFinalRowNeverLeavesTheGroupReady) {
  api.SubReply("TFtest",0,false);
  api.spi->OnRspSubMarketData(nullptr,nullptr,0,true);
  EXPECT_TRUE(source->failed());
  EXPECT_FALSE(source->ready());
}

TEST_F(MultiMarketTest, LateSubscriptionFailureInvalidatesAlreadyQueuedQuotes) {
  for (const auto& name : api.symbols) api.SubReply(name.c_str());
  ASSERT_TRUE(source->ready());
  api.Tick("TFtest");
  api.SubReply("Ttest",9);
  EXPECT_FALSE(source->ready());
  EXPECT_FALSE(source->TryRead(quote));
}

TEST(MultiMarketConfig, ConflictingMappingsAreRejectedBeforeCreatingSdk) {
  SimNowMarketConfig config;
  config.front = "tcp://fixture:1";
  config.anonymous = true;
  const std::vector<std::vector<SimNowSubscription>> invalid{
      {{"A",1},{"A",1}}, {{"A",1},{"A",2}}, {{"A",1},{"B",1}},
      {{"",1}}, {{"A",0}}, {{std::string(1024,'x'),1}}};
  auto unexpected = []() -> CThostFtdcMdApi* { ADD_FAILURE(); return nullptr; };
  for (const auto& entries : invalid) {
    config.subscriptions = entries;
    EXPECT_FALSE(internal::StartSession(config,unexpected));
  }
  config.subscriptions = {{"A",1}};
  config.symbol = "B";
  config.instrument_id = 2;
  EXPECT_FALSE(internal::StartSession(config,unexpected));
}

TEST_F(MultiMarketTest, GroupSharesOneRingWhileEachInstrumentKeepsItsSequence) {
  for (const auto& name : api.symbols) api.SubReply(name.c_str());
  for (int i=0;i<800;++i) api.Tick(i%2 ? "TFtest" : "Ttest");
  for (int i=0;i<700;++i) {
    ASSERT_TRUE(source->TryRead(quote));
    EXPECT_EQ(quote.instrument_id,i%2 ? 42 : 81);
    EXPECT_EQ(quote.sequence,static_cast<unsigned>(i/2+1));
  }
  for (int i=800;i<1600;++i) api.Tick(i%2 ? "TFtest" : "Ttest");
  for (int i=700;i<1600;++i) {
    ASSERT_TRUE(source->TryRead(quote));
    EXPECT_EQ(quote.instrument_id,i%2 ? 42 : 81);
    EXPECT_EQ(quote.sequence,static_cast<unsigned>(i/2+1));
  }
  EXPECT_FALSE(source->TryRead(quote));
}

}  // namespace
}  // namespace yquant::market
