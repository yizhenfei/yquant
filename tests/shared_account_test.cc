#include "yquant/runtime/monod.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/market/local_market_client.h"
#include "yquant/market/market_service.h"
#include "yquant/strategy/strategy_service.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::runtime {
namespace {
using namespace trading;

// Publishes one caller-controlled quote through the production market service.
class Source final : public market::QuoteSource {
 public:
  model::Quote next;
  bool available = false;
  // market::QuoteSource implementation.
  bool TryRead(model::Quote& quote) override {
    if (!available) return false;
    quote = next;
    available = false;
    return true;
  }
};

// Parses only public observations written by each independently loaded plugin.
struct View {
  bool cash = false, positions = false;
  double available = 0, reserved = 0, commission = 0, pnl = 0;
  int quantity = 0, frozen = 0;
  bool operator==(const View&) const = default;
};

class SharedAccountTest : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    char path[] = "/tmp/yquant-shared-XXXXXX";
    ASSERT_NE(mkdtemp(path), nullptr);
    root = path;
    ASSERT_TRUE(trading.BindAccount(7, counter));
    ASSERT_TRUE(trading.BindAccount(8, other));
    FundsTerms terms;
    terms.currency = model::Currency("CNY");
    terms.contract_multiplier = 1;
    terms.margin_rate = 0.1;
    for (auto& fee : terms.fees) fee = FeeRate{1, 0};
    for (int account : {7, 8}) {
      for (int instrument : {42, 81}) {
        for (auto side : {Side::kBuy, Side::kSell}) {
          ASSERT_TRUE(trading.ConfigureFundsTerms(account, instrument, side, terms));
        }
      }
    }
    ASSERT_TRUE(registry.Register(1, trading));
    client = TradingClientFactory(registry).Create(1);
    ASSERT_TRUE(markets.Register(1, market));
    other.SetInitialState({terms.currency, 1000, 1000, 0, 0}, {});
    deployment = "market_front=tcp://fixture:1\ntrading_front=tcp://fixture:2\n"
        "instrument.42=TEST first\ninstrument.81=TEST second\n";
  }
  void TearDown() override {
    groups.clear();
    std::filesystem::remove_all(root);
  }
  void Add(const std::string& name, int caller, int strategy, int instrument,
           const std::string& submit, const std::string& cancel,
           bool close = false, int quantity = 1) {
    deployment += "strategy." + std::to_string(caller) + "." + std::to_string(strategy)
                  + "=" + name + ".conf\n";
    std::ofstream(root / (name + ".conf")) << "library=" << PROBE_PLUGIN
        << "\nstrategy_id=" << strategy << "\naccount_id=7\nparam.instrument_id=" << instrument
        << "\nparam.output=" << (root / name).string()
        << "\nparam.submit=" << submit << "\nparam.cancel=" << cancel
        << "\nparam.offset=" << (close ? "close" : "open")
        << "\nparam.quantity=" << quantity << '\n';
    names.push_back(name);
  }
  void Load(double balance, std::vector<PositionInfo> positions = {}, double margin = 0) {
    counter.SetInitialState({model::Currency("CNY"), balance, balance - margin, margin, 0}, positions);
    std::ofstream(root / "runtime.conf") << deployment;
    MonodConfig config;
    ASSERT_TRUE(ReadMonodConfig((root / "runtime.conf").string(), config, error)) << error;
    for (const auto& group : config.services) {
      auto service = std::make_unique<strategy::StrategyService>(group.caller_service_id, market_factory, 1, trading_factory, 1);
      ASSERT_TRUE(service->Load(group.instances, error)) << error;
      groups.push_back(std::move(service));
    }
  }
  void Publish(std::uint64_t sequence, double price = 100) {
    for (std::uint32_t id : {42, 81}) {
      source.next = {id, price, price + 1, 1, 1, sequence};
      source.available = true;
      market.Run();
    }
  }
  void Step(std::uint64_t sequence, double price = 100) {
    Publish(sequence, price);
    trading.Run();
    Publish(900);  // Query after every instance's operation and all callbacks.
  }
  View Latest(const std::string& name) {
    std::ifstream file(root / name);
    std::string line;
    View result;
    while (std::getline(file, line)) {
      if (!line.starts_with("view ")) continue;
      std::istringstream row(line.substr(5));
      EXPECT_TRUE(static_cast<bool>(row >> result.cash >> result.positions >> result.available
          >> result.reserved >> result.commission >> result.pnl >> result.quantity >> result.frozen));
    }
    return result;
  }
  View Shared() {
    const auto first = Latest(names.front());
    for (const auto& name : names) EXPECT_EQ(Latest(name), first) << name;
    return first;
  }
  int EventCount(const std::string& name, const std::string& expected) {
    std::ifstream file(root / name);
    std::string line;
    int count = 0;
    while (std::getline(file, line)) if (line == expected) ++count;
    return count;
  }
  void Failure(const std::string& name, OrderId id, int strategy, FailureReason reason) {
    EXPECT_EQ(EventCount(name, "failure " + std::to_string(id.caller_service_id) + " "
        + std::to_string(id.sequence) + " " + std::to_string(strategy) + " "
        + std::to_string(static_cast<int>(reason))), 1);
  }
  int CallerB() const { return GetParam() ? 20 : 10; }
  OrderId IdB(int sequence) const { return {static_cast<std::uint64_t>(CallerB()),
      static_cast<std::uint64_t>(GetParam() ? sequence : sequence + 1)}; }

  std::filesystem::path root;
  std::string deployment, error;
  std::vector<std::string> names;
  Source source;
  market::MarketService market{source};
  SimulatedTradingAdapter counter, other;
  TradingService trading;
  TradingServiceRegistry registry;
  market::MarketServiceRegistry markets;
  market::MarketClientFactory market_factory{markets};
  TradingClientFactory trading_factory{registry};
  std::unique_ptr<TradingClient> client;
  std::vector<std::unique_ptr<strategy::StrategyService>> groups;
};

TEST_P(SharedAccountTest, FundsCompetitionReleaseAndIndependentOtherAccount) {
  Add("a", 10, 11, 42, "1", "3");
  Add("b", 10, 12, 42, "1", "0");
  Add("c", CallerB(), 13, 81, "2 4 5 6", "7");
  Load(25);
  Publish(900);
  EXPECT_FALSE(Shared().cash);
  EXPECT_FALSE(Shared().positions);
  trading.Run();
  Step(1);
  EXPECT_DOUBLE_EQ(Shared().available, 3);
  EXPECT_DOUBLE_EQ(Shared().reserved, 22);
  Step(2);
  const OrderId c1{static_cast<std::uint64_t>(CallerB()), GetParam() ? 1U : 3U};
  Failure("c", c1, 13, FailureReason::kInsufficientFunds);
  Step(3);
  EXPECT_DOUBLE_EQ(Shared().available, 14);
  counter.SetNextSubmit({SendResult::kNotSent, std::nullopt});
  Step(4);
  auto c2 = c1; ++c2.sequence;
  Failure("c", c2, 13, FailureReason::kSendFailed);
  EXPECT_DOUBLE_EQ(Shared().available, 14);
  counter.SetNextSubmit({SendResult::kSent, AdapterOrderState::kRejected});
  Step(5);
  auto c3 = c2; ++c3.sequence;
  EXPECT_DOUBLE_EQ(Shared().available, 14);
  counter.Emit({c3, AdapterOrderState::kRejected});
  Step(6);
  EXPECT_DOUBLE_EQ(Shared().available, 3);
  const TradeInfo fill{{10, 2}, {"TEST", "day", "one"}, 100, 1};
  ASSERT_TRUE(counter.Fill(fill));
  trading.Run();
  counter.EmitTrade(fill);
  Step(900);
  EXPECT_EQ(Shared().quantity, 1);
  EXPECT_DOUBLE_EQ(Shared().commission, 1);
  EXPECT_DOUBLE_EQ(Shared().available, 3);
  EXPECT_EQ(EventCount("b", "trade 10 2 12 1"), 1);
  EXPECT_EQ(EventCount("a", "trade 10 2 11 1"), 0);
  Step(7);
  auto c4 = c3; ++c4.sequence;
  counter.Emit({c4, AdapterOrderState::kCanceled});
  Step(900);
  EXPECT_DOUBLE_EQ(Shared().available, 14);
  EXPECT_DOUBLE_EQ(Shared().reserved, 0);
  const auto before = Shared();
  // Keeps the second account's callbacks separate from the loaded strategies.
  struct Listener final : TradingListener {
    void OnRequestFailure(const RequestFailure&) override {}
    void OnOrderReport(const OrderReport&) override {}
    void OnCancelReport(const CancelReport&) override {}
    void OnTradeReport(const TradeReport&) override {}
  } listener;
  ASSERT_TRUE(client->BindListener(listener));
  client->SubmitOrder({{{99, 1}, 8, 1, 81, Side::kBuy, Offset::kOpen, 100, 2}});
  trading.Run();
  ASSERT_TRUE(other.Fill({{99, 1}, {"TEST", "day", "other"}, 100, 2}));
  trading.Run();
  Publish(900);
  EXPECT_EQ(Shared(), before);
  FundsSnapshot isolated;
  ASSERT_TRUE(client->QueryFunds({8}, isolated));
  EXPECT_DOUBLE_EQ(isolated.available_for_orders(), 978);
  EXPECT_TRUE(client->Close());
}

TEST_P(SharedAccountTest, CloseCompetitionFillAndCancelReleaseOneSharedPosition) {
  Add("a", 10, 11, 42, "1", "3", true, 2);
  Add("b", CallerB(), 12, 42, "2 4", "5", true, 2);
  Load(1000, {{{42, PositionSide::kLong}, 3, 0, 0, 0, PositionCost{300, 30}}}, 30);
  trading.Run();
  Step(1, 110);
  EXPECT_EQ(Shared().frozen, 2);
  Step(2, 110);
  Failure("b", IdB(1), 12, FailureReason::kInsufficientPosition);
  const TradeInfo fill{{10, 1}, {"TEST", "day", "close"}, 110, 1};
  ASSERT_TRUE(counter.Fill(fill));
  trading.Run();
  counter.EmitTrade(fill);
  Step(900);
  EXPECT_EQ(Shared().quantity, 2);
  EXPECT_EQ(Shared().frozen, 1);
  EXPECT_DOUBLE_EQ(Shared().pnl, 10);
  EXPECT_DOUBLE_EQ(Shared().commission, 1);
  Step(3);
  EXPECT_EQ(Shared().frozen, 0);
  EXPECT_DOUBLE_EQ(Shared().available, 989);
  counter.Emit({{10, 1}, AdapterOrderState::kCanceled, 1});
  Step(4, 110);
  EXPECT_EQ(Shared().frozen, 2);
  ASSERT_TRUE(counter.Fill({IdB(2), {"TEST", "day", "second-close"}, 110, 1}));
  trading.Run();
  Step(5);
  EXPECT_EQ(Shared().quantity, 1);
  EXPECT_EQ(Shared().frozen, 0);
  EXPECT_DOUBLE_EQ(Shared().pnl, 20);
  EXPECT_DOUBLE_EQ(Shared().commission, 2);
}

TEST_P(SharedAccountTest, UnpricedAndFailedAccountingAreUnavailableToEveryInstance) {
  Add("a", 10, 11, 42, "1 3", "0", false, 2);
  Add("b", CallerB(), 12, 81, "2", "0");
  Load(1000);
  trading.Run();
  Step(1);
  counter.Emit({{10, 1}, AdapterOrderState::kAccepted, 1});
  Step(900);
  EXPECT_FALSE(Shared().cash);
  EXPECT_FALSE(Shared().positions);
  Step(2);
  Failure("b", IdB(1), 12, FailureReason::kFundsUnavailable);
  counter.EmitTrade({{10, 1}, {"TEST", "day", "priced"}, 100, 1});
  Step(900);
  EXPECT_TRUE(Shared().cash);
  EXPECT_TRUE(Shared().positions);
  Step(3);
  const OrderId next{10, GetParam() ? 2U : 3U};
  counter.EmitTrade({next, {"TEST", "day", "overflow"}, std::numeric_limits<double>::max(), 2});
  Step(900);
  EXPECT_FALSE(Shared().cash);
  EXPECT_FALSE(Shared().positions);
}

INSTANTIATE_TEST_SUITE_P(Layouts, SharedAccountTest, ::testing::Bool());
}  // namespace
}  // namespace yquant::runtime
