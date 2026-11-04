#include "yquant/strategy/loaded_strategy.h"

#include <dlfcn.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/market/market_client_factory.h"
#include "yquant/market/market_service.h"
#include "yquant/market/market_service_registry.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::strategy {
namespace {

// Presents one quote per explicit test step through the real market service.
class QuoteInput final : public market::QuoteSource {
 public:
  std::optional<model::Quote> pending;
  // market::QuoteSource implementation.
  market::QuoteReadResult Read(model::Quote& quote) override {
    if (!pending) return market::QuoteReadResult::kIdle;
    quote = *pending;
    pending.reset();
    return market::QuoteReadResult::kQuote;
  }
};

// Keeps submitted requests visible while using the existing simulated adapter.
class Counter final : public trading::ITradingAdapter {
 public:
  trading::SimulatedTradingAdapter simulated;
  std::vector<trading::OrderInfo> orders;
  // trading::ITradingAdapter implementation.
  void SetCallback(trading::ITradingAdapterCallback& c) override { simulated.SetCallback(c); }
  bool StartAccountSync(std::uint64_t id) override { return simulated.StartAccountSync(id); }
  trading::SendResult SubmitOrder(const trading::SubmitOrderRequest& r) override {
    orders.push_back(r.order);
    return simulated.SubmitOrder(r);
  }
  trading::SendResult CancelOrder(const trading::AdapterCancelOrderRequest& r) override {
    return simulated.CancelOrder(r);
  }
  void Run() override { simulated.Run(); }
};

class PluginTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char path[] = "/tmp/yquant-plugin-XXXXXX";
    ASSERT_NE(mkdtemp(path), nullptr);
    root = path;
    ASSERT_TRUE(trading.BindAccount(7, counter));
    trading::FundsTerms terms;
    terms.currency = model::Currency("CNY");
    terms.contract_multiplier = 1;
    terms.margin_rate = 0.1;
    for (auto& fee : terms.fees) fee = trading::FeeRate{};
    for (auto side : {trading::Side::kBuy, trading::Side::kSell}) {
      ASSERT_TRUE(trading.ConfigureFundsTerms(7, 42, side, terms));
    }
    counter.simulated.SetInitialState({terms.currency, 1000, 1000, 0, 0}, {});
    trading.Run();
    ASSERT_TRUE(markets.Register(1, market));
    market_client = market::MarketClientFactory(markets).Create(1);
    ASSERT_TRUE(traders.Register(2, trading));
    trading_client = trading::TradingClientFactory(traders).Create(2);
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  std::string Config(std::string library, std::string params, int strategy = 11) {
    auto file = root / (std::to_string(strategy) + ".conf");
    std::ofstream out(file);
    out << "# test configuration\nlibrary=" << library
        << "\nstrategy_id=" << strategy << "\naccount_id=7\ninstrument_id=42\n" << params;
    out.close();
    return file.string();
  }
  auto Load(const std::string& path) {
    return LoadedStrategy::Load(path, *market_client, *trading_client, ids, error);
  }
  void Quote(double bid = 100) {
    input.pending = model::Quote{42, bid, 102, 1, 1, 1};
    market.Run();
  }
  bool Unloaded(const char* path) {
    void* library = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    if (library) dlclose(library);
    return library == nullptr;
  }
  std::filesystem::path root;
  QuoteInput input;
  market::MarketService market{input};
  market::MarketServiceRegistry markets;
  Counter counter;
  trading::TradingService trading;
  trading::TradingServiceRegistry traders;
  trading::OrderIdGenerator ids{10};
  std::unique_ptr<market::MarketClient> market_client;
  std::unique_ptr<trading::TradingClient> trading_client;
  std::string error;
};

TEST_F(PluginTest, SameHostLoadsDifferentStrategiesAndParametersFromFiles) {
  const auto relative = std::filesystem::relative(MAKER_PLUGIN, root).string();
  auto maker = Load(Config(relative, "param.quantity=2\n"));
  ASSERT_TRUE(maker) << error;
  auto fixed = Load(Config(FIXED_PLUGIN, "param.price=90\n", 12));
  ASSERT_TRUE(fixed) << error;
  Quote();
  trading.Run();
  ASSERT_EQ(counter.orders.size(), 3);
  EXPECT_EQ(counter.orders[0].strategy_id, 11);
  EXPECT_EQ(counter.orders[0].quantity, 2);
  EXPECT_EQ(counter.orders[0].price, 100);
  EXPECT_EQ(counter.orders[1].side, trading::Side::kSell);
  EXPECT_EQ(counter.orders[2].strategy_id, 12);
  EXPECT_EQ(counter.orders[2].price, 90);
  EXPECT_EQ(counter.orders[2].account_id, 7);
  EXPECT_EQ(counter.orders[2].instrument_id, 42);
  EXPECT_EQ(counter.orders[2].order_id.sequence, 3);
  EXPECT_TRUE(maker->Stop());
  EXPECT_TRUE(maker->Stop());
  fixed.reset();
  EXPECT_TRUE(Unloaded(MAKER_PLUGIN));
  EXPECT_TRUE(Unloaded(FIXED_PLUGIN));
  Quote(99);
  trading.Run();
  EXPECT_EQ(counter.orders.size(), 3);
}

TEST_F(PluginTest, UnloadDetachesQueuedFailuresAndLateOrderReports) {
  auto marker = (root / "destruction").string();
  auto plugin = Load(Config(FIXED_PLUGIN, "param.price=90\nparam.marker=" + marker));
  ASSERT_TRUE(plugin) << error;
  counter.simulated.SetNextSubmit({trading::SendResult::kNotSent,
                                   trading::AdapterOrderState::kAccepted});
  Quote();
  ASSERT_EQ(counter.orders.size(), 1);
  const auto submitted = counter.orders[0];
  plugin.reset();
  EXPECT_TRUE(std::filesystem::exists(marker));
  EXPECT_TRUE(Unloaded(FIXED_PLUGIN));
  // Includes a queued service failure and an adapter event after dlclose.
  trading.Run();
  counter.simulated.Emit({submitted.order_id, trading::AdapterOrderState::kAccepted});
  trading::OrderSnapshot state;
  ASSERT_TRUE(trading_client->QueryOrder({submitted.order_id, 7, 11}, state));
  EXPECT_EQ(state.state, trading::OrderState::kAccepted);
  Quote();
  EXPECT_EQ(counter.orders.size(), 1);
}

TEST_F(PluginTest, UnloadRemovesOutstandingCancelCallbacks) {
  auto maker = Load(Config(MAKER_PLUGIN, "param.quantity=2"));
  ASSERT_TRUE(maker) << error;
  Quote();
  trading.Run();
  Quote(99);  // Queues the cancellation of the working bid.
  maker.reset();
  EXPECT_TRUE(Unloaded(MAKER_PLUGIN));
  trading.Run();
  trading::OrderSnapshot state;
  ASSERT_TRUE(trading_client->QueryOrder({counter.orders[0].order_id,7,11}, state));
  EXPECT_EQ(state.state, trading::OrderState::kCanceled);
  EXPECT_EQ(counter.orders.size(), 2);
}

TEST_F(PluginTest, RejectsBadLibraryEntryVersionAndCreationWithoutResidualCallbacks) {
  for (const char* library : {"/no-such-strategy.so", NO_ENTRY_PLUGIN, BAD_VERSION_PLUGIN,
                              FAIL_CREATE_PLUGIN, MAKER_PLUGIN}) {
    auto marker = (root / std::filesystem::path(library).filename()).string();
    auto strategy = Load(Config(library, "param.price=91\nparam.marker=" + marker));
    EXPECT_FALSE(strategy);
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(Unloaded(library));
    if (std::string(library) == FAIL_CREATE_PLUGIN) {
      EXPECT_TRUE(std::filesystem::exists(marker));
    }
    Quote();
    trading.Run();
    EXPECT_TRUE(counter.orders.empty());
  }
  auto valid = Load(Config(MAKER_PLUGIN, "param.quantity=1"));
  ASSERT_TRUE(valid) << error;
}

TEST_F(PluginTest, InvalidConfigurationDoesNotChangeOutputOrLoadLibrary) {
  StrategyConfig config;
  config.library = "unchanged";
  for (const char* text : {"library=x\n", "strategy_id=1\nstrategy_id=2",
                           "instrument_id=4294967296", "account_id=-1", "typo=1",
                           "param.=1"}) {
    const auto file = root / "invalid.conf";
    std::ofstream(file) << text;
    EXPECT_FALSE(ReadStrategyConfig(file.string(), config, error));
    EXPECT_EQ(config.library, "unchanged");
  }
}

}  // namespace
}  // namespace yquant::strategy
