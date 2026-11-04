#include "yquant/market/simnow_quote_source.h"

#include <limits>
#include <thread>
#include <vector>

#include "fake_md_api.h"
#include "gtest/gtest.h"
#include "simnow_session.h"
#include "strategies/quote_strategy.h"
#include "yquant/market/market_client_factory.h"
#include "yquant/market/market_service_registry.h"

namespace yquant::market {
namespace {

const SimNowMarketConfig kConfig{"tcp://fake:1", "IFtest", "broker", "user",
                                 "password", 42};

// Drives the native SPI manually; TryRead advances SDK requests on this thread.
class MarketSessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    source = internal::StartSession(kConfig, [&] { return &api; });
    ASSERT_NE(source, nullptr);
  }
  void Login() {
    api.spi->OnFrontConnected();
    source->TryRead(unused);
  }
  void Subscribe() {
    Login();
    api.LoginReply();
    source->TryRead(unused);
  }
  test::FakeMdApi api;
  std::unique_ptr<SimNowQuoteSource> source;
  model::Quote unused{};
};

// Captures values and the thread that executed each strategy-side callback.
class Capture final : public QuoteListener, public examples::QuoteIntentListener {
 public:
  // QuoteListener implementation.
  void OnQuote(const model::Quote& q) override { quotes.push_back(q); }
  // QuoteIntentListener implementation.
  void OnQuoteIntent(const examples::QuoteIntent& q) override {
    intents.push_back(q);
    threads.push_back(std::this_thread::get_id());
  }
  std::vector<model::Quote> quotes;
  std::vector<examples::QuoteIntent> intents;
  std::vector<std::thread::id> threads;
};

TEST_F(MarketSessionTest, WorkerCallbacksReachSubscribedStrategyOnBusinessThread) {
  const auto business = std::this_thread::get_id();
  std::thread([&] { api.spi->OnFrontConnected(); }).join();
  source->TryRead(unused);
  EXPECT_EQ(api.front, "tcp://fake:1");
  EXPECT_STREQ(api.login.UserID, "user");
  EXPECT_STREQ(api.login.BrokerID, "broker");
  EXPECT_STREQ(api.login.Password, "password");
  EXPECT_EQ(api.login_calls, 1);
  std::thread([&] { api.LoginReply(); }).join();
  source->TryRead(unused);
  EXPECT_EQ(api.symbol, "IFtest");
  EXPECT_EQ(api.login_thread, business);
  EXPECT_EQ(api.sub_thread, business);
  MarketService service(*source);
  MarketServiceRegistry registry;
  ASSERT_TRUE(registry.Register(7, service));
  auto client = MarketClientFactory(registry).Create(7);
  ASSERT_NE(client, nullptr);
  Capture seen, other;
  examples::QuoteStrategy strategy(seen);
  client->Subscribe(42, seen);
  client->Subscribe(42, strategy);
  client->Subscribe(99, other);
  std::thread([&] {
    api.Tick("IFtest");  // Before subscription confirmation.
    api.SubReply("IFtest");
    api.Tick("other");
    api.Tick("IFtest", 100, std::numeric_limits<double>::max());
    api.Tick("IFtest", std::numeric_limits<double>::quiet_NaN());
    api.Tick("IFtest", 100, 100);
    api.Tick("IFtest", 100, 101, 0);
    api.Tick("IFtest", 100.25, 100.5, 8);
    api.Tick("IFtest", 101, 102, 9);
  }).join();
  EXPECT_TRUE(seen.quotes.empty());
  EXPECT_TRUE(seen.intents.empty());
  service.Run();
  ASSERT_EQ(seen.quotes.size(), 2U);
  ASSERT_EQ(seen.intents.size(), 2U);
  EXPECT_TRUE(other.quotes.empty());
  EXPECT_DOUBLE_EQ(seen.quotes[0].bid_price, 100.25);
  EXPECT_DOUBLE_EQ(seen.quotes[0].ask_price, 100.5);
  EXPECT_EQ(seen.quotes[0].bid_quantity, 8);
  EXPECT_EQ(seen.quotes[0].ask_quantity, 5);
  EXPECT_EQ(seen.quotes[1].bid_quantity, 9);
  for (std::size_t i = 0; i != 2; ++i) {
    EXPECT_EQ(seen.threads[i], business);
    EXPECT_EQ(seen.intents[i].instrument_id, 42U);
    EXPECT_EQ(seen.intents[i].sequence, i + 1);
    EXPECT_DOUBLE_EQ(seen.intents[i].bid_price, seen.quotes[i].bid_price);
    EXPECT_DOUBLE_EQ(seen.intents[i].ask_price, seen.quotes[i].ask_price);
  }
  client->Unsubscribe(42, strategy);
  client->Unsubscribe(42, seen);
  client->Unsubscribe(99, other);
}

TEST_F(MarketSessionTest, LoginFailureIsTerminal) {
  Login();
  api.LoginReply(3);
  api.LoginReply();
  source->TryRead(unused);
  EXPECT_TRUE(source->failed());
  EXPECT_EQ(api.sub_calls, 0);
}

TEST_F(MarketSessionTest, FailedRequestDoesNotRetryOnEveryRead) {
  api.login_rc = -1;
  Login();
  source->TryRead(unused);
  EXPECT_TRUE(source->failed());
  EXPECT_EQ(api.login_calls, 1);
}

TEST_F(MarketSessionTest, SubscriptionErrorPreventsDelivery) {
  Subscribe();
  api.SubReply("IFtest", 7);
  api.Tick("IFtest");
  EXPECT_TRUE(source->failed());
  EXPECT_FALSE(source->TryRead(unused));
}

TEST_F(MarketSessionTest, WrongLastContractCannotMakeSessionReady) {
  Subscribe();
  api.SubReply("other");
  EXPECT_TRUE(source->failed());
}

TEST_F(MarketSessionTest, DisconnectInvalidatesQueuedQuotesAndStopsReconnect) {
  Subscribe();
  api.SubReply("IFtest");
  api.Tick("IFtest");
  api.spi->OnFrontDisconnected(123);
  api.spi->OnFrontConnected();
  model::Quote sentinel{99, 10, 11, 1, 2, 9};
  EXPECT_FALSE(source->TryRead(sentinel));
  EXPECT_EQ(sentinel.instrument_id, 99U);
  EXPECT_TRUE(source->failed());
  EXPECT_EQ(api.login_calls, 1);
}

TEST_F(MarketSessionTest, BoundedQueueFailsInsteadOfOverwritingUnreadQuotes) {
  Subscribe();
  api.SubReply("IFtest");
  for (int i = 0; i < 1024; ++i) api.Tick("IFtest");
  EXPECT_FALSE(source->failed());
  api.Tick("IFtest");
  EXPECT_TRUE(source->failed());
  EXPECT_FALSE(source->TryRead(unused));
}

TEST_F(MarketSessionTest, IgnoresUnrelatedLoginAndWaitsForFinalReply) {
  Login();
  CThostFtdcRspUserLoginField reply{};
  api.spi->OnRspUserLogin(&reply, nullptr, 2, true);
  api.spi->OnRspUserLogin(&reply, nullptr, 1, false);
  source->TryRead(unused);
  EXPECT_EQ(api.sub_calls, 0);
  api.LoginReply();
  api.sub_rc = -2;
  source->TryRead(unused);
  EXPECT_EQ(api.sub_calls, 1);
  EXPECT_TRUE(source->failed());
}

TEST_F(MarketSessionTest, ZeroIdFinalLoginReplyStartsTheSingleSubscription) {
  Login();
  CThostFtdcRspUserLoginField reply{};
  api.spi->OnRspUserLogin(&reply, nullptr, 0, false);
  source->TryRead(unused);
  EXPECT_EQ(api.sub_calls, 0);
  api.spi->OnRspUserLogin(&reply, nullptr, 0, true);
  source->TryRead(unused);
  EXPECT_EQ(api.sub_calls, 1);
  EXPECT_EQ(api.symbol, "IFtest");
}

TEST_F(MarketSessionTest, RingWrapPreservesArrivalOrderAndQuantity) {
  Subscribe();
  api.SubReply("IFtest");
  for (int i = 1; i <= 800; ++i) api.Tick("IFtest", 100, 101, i);
  auto read = [&](int first, int last) {
    for (int i = first; i <= last; ++i) {
      ASSERT_TRUE(source->TryRead(unused));
      EXPECT_EQ(unused.sequence, static_cast<std::uint64_t>(i));
      EXPECT_EQ(unused.bid_quantity, i);
    }
  };
  read(1, 700);
  for (int i = 801; i <= 1600; ++i) api.Tick("IFtest", 100, 101, i);
  read(701, 1600);
  EXPECT_FALSE(source->TryRead(unused));
}

TEST_F(MarketSessionTest, DestructionUnregistersSpiBeforeRelease) {
  source.reset();
  EXPECT_TRUE(api.released_without_spi);
}

TEST(MarketConfig, ExplicitAnonymousLoginDoesNotForwardAccountCredentials) {
  test::FakeMdApi api;
  auto config = kConfig;
  config.anonymous = true;
  auto source = internal::StartSession(config, [&] { return &api; });
  ASSERT_NE(source, nullptr);
  api.spi->OnFrontConnected();
  model::Quote unused{};
  source->TryRead(unused);
  EXPECT_EQ(api.login_calls, 1);
  EXPECT_STREQ(api.login.BrokerID, "");
  EXPECT_STREQ(api.login.UserID, "");
  EXPECT_STREQ(api.login.Password, "");
}

TEST(MarketConfig, InvalidCredentialsAreNotTruncatedOrPassedToTheSdk) {
  auto config = kConfig;
  std::string overlong(1024, 'x');
  config.password = overlong;
  bool created = false;
  EXPECT_EQ(internal::StartSession(config, [&]() -> CThostFtdcMdApi* {
    created = true;
    return nullptr;
  }), nullptr);
  EXPECT_FALSE(created);
}

}  // namespace
}  // namespace yquant::market
