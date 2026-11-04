#include "yquant/runtime/monod.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_service.h"

namespace yquant::runtime {
namespace {
using namespace trading;

// Provides the same adapter interface as production while controlling native
// readiness and trade events. Requests originate in the loaded strategy.
class Counter final : public SimNowTradingAdapter {
 public:
  Counter() {
    sim.SetInitialState({model::Currency("CNY"), 10000, 10000, 0, 0}, {});
  }
  SimulatedTradingAdapter sim;
  TraderLoginStatus status;
  std::vector<OrderInfo> orders;
  std::vector<AdapterCancelOrderRequest> cancels;
  ITradingAdapterCallback* sink = nullptr;
  std::uint64_t sync = 0;
  int runs = 0, fills = 0, early_submits = 0;
  bool started = false, reject_shutdown = false, reject_first_cancel = false;
  // SimNowTradingAdapter implementation.
  const TraderLoginStatus& login_status() const override { return status; }
  bool StartSnapshot(std::string_view) override { return false; }
  SnapshotStatus snapshot_status() const override { return {}; }
  const SimNowStartupSnapshot* snapshot() const override { return nullptr; }
  std::optional<SimNowNativeOrderKey> NativeKey(OrderId) const override { return std::nullopt; }

  // ITradingAdapter implementation.
  void SetCallback(ITradingAdapterCallback& callback) override {
    sink = &callback;
    sim.SetCallback(callback);
  }
  bool StartAccountSync(std::uint64_t id) override { sync = id; return true; }
  SendResult SubmitOrder(const SubmitOrderRequest& request) override {
    if (!started) ++early_submits;
    orders.push_back(request.order);
    return sim.SubmitOrder(request);
  }
  SendResult CancelOrder(const AdapterCancelOrderRequest& request) override {
    cancels.push_back(request);
    if (reject_first_cancel && cancels.size() == 1) {
      sim.SetNextCancel({SendResult::kSent, CancelResult::kRejected});
    }
    if (reject_shutdown && runs > 10) sim.SetNextCancel({SendResult::kSent, CancelResult::kRejected});
    return sim.CancelOrder(request);
  }
  void Run() override {
    ++runs;
    if (runs < 4) return;
    status.phase = LoginPhase::kLoggedIn;
    if (!started) {
      // The live adapter also installs the configured instrument's order terms.
      FundsTerms terms;
      terms.currency = model::Currency("CNY");
      terms.contract_multiplier = 1;
      terms.margin_rate = 0.1;
      for (auto& fee : terms.fees) fee = FeeRate{1,0};
      AccountRecovery baseline;
      baseline.funds = {terms.currency, 10000,10000,0,0};
      baseline.order_terms.emplace(42, std::pair{terms,terms});
      sink->OnAccountRecovery(sync, std::move(baseline));
      sink->OnAccountSyncReport({sync, AccountSyncResult::kComplete});
      started = true;
    }
    sim.Run();
    if (runs == 9 && !orders.empty()) {
      const auto order = orders.back();
      if (sim.Fill({order.order_id, {"TEST","day","fill"}, order.price, 1})) ++fills;
      sim.Run();
    }
  }
};

// Drops an early valid quote before trade readiness, then supplies stable and
// changed prices on distinct scheduling rounds.
class Quotes final : public market::SimNowQuoteSource {
 public:
  explicit Quotes(Counter& counter) : counter(counter) {}
  Counter& counter;
  int last = -1;
  bool broken = false;
  // SimNowQuoteSource implementation.
  bool failed() const override { return broken; }
  bool ready() const override { return !broken && counter.started; }
  // QuoteSource implementation.
  market::QuoteReadResult Read(model::Quote& q) override {
    if (last == counter.runs) return market::QuoteReadResult::kIdle;
    last = counter.runs;
    q = {42, counter.runs < 4 ? 50.0 : counter.runs < 6 ? 100.0 : 101.0,
         103,1,1,static_cast<std::uint64_t>(counter.runs)};
    return market::QuoteReadResult::kQuote;
  }
};

class MonodRuntimeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char path[] = "/tmp/yquant-runtime-XXXXXX";
    ASSERT_NE(mkdtemp(path), nullptr);
    root = path;
    std::ofstream(root / "strategy.conf") << "library=" << MAKER_PLUGIN
        << "\nstrategy_id=11\naccount_id=7\ninstrument_id=42\nparam.quantity=1\n";
    std::ofstream(root / "runtime.conf") << "strategy=strategy.conf\n"
        "market_front=tcp://fixture:1\ntrading_front=tcp://fixture:2\n"
        "exchange=TEST\nsymbol=contract\ncaller_service_id=10\n"
        "startup_seconds=1\nshutdown_seconds=1\nrun_seconds=1\n";
    ASSERT_TRUE(ReadMonodConfig((root / "runtime.conf").string(), config, error)) << error;
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  std::filesystem::path root;
  MonodConfig config;
  std::string error;
};

TEST_F(MonodRuntimeTest, ConfiguredPluginTradesOnlyAfterReadinessAndDrainsOnExit) {
  Counter counter;
  Quotes quotes(counter);
  ::testing::internal::CaptureStdout();
  const int rc = RunMonod(config, quotes, counter, [&] { return counter.runs > 12; }, error);
  const auto out = ::testing::internal::GetCapturedStdout();
  ASSERT_EQ(rc, 0) << error;
  EXPECT_EQ(counter.early_submits, 0);
  ASSERT_GE(counter.orders.size(), 4);
  EXPECT_EQ(counter.orders[0].price, 100);
  EXPECT_EQ(counter.orders[0].strategy_id, 11);
  EXPECT_EQ(counter.orders[0].account_id, 7);
  EXPECT_EQ(counter.orders[0].instrument_id, 42);
  EXPECT_EQ(counter.orders[0].order_id.caller_service_id, 10);
  EXPECT_EQ(counter.orders[1].price, 103);
  EXPECT_EQ(counter.fills, 1);
  EXPECT_GE(counter.cancels.size(), 3);
  EXPECT_NE(out.find("monod_started strategy=11 account=7 instrument=42"), out.npos);
  EXPECT_NE(out.find("remaining=0 commission=1"), out.npos);
}

TEST_F(MonodRuntimeTest, ShutdownSkipsAlreadyUsedCancelIdsWithoutResubmittingOrders) {
  Counter counter;
  counter.reject_first_cancel = true;
  Quotes quotes(counter);
  EXPECT_EQ(RunMonod(config, quotes, counter, [&] { return counter.runs > 12; }, error), 0)
      << error;
  EXPECT_GE(counter.cancels.size(), 3);
}

TEST_F(MonodRuntimeTest, RejectedShutdownIsNotReportedAsClean) {
  Counter counter;
  counter.reject_shutdown = true;
  Quotes quotes(counter);
  EXPECT_EQ(RunMonod(config, quotes, counter, [&] { return counter.runs > 12; }, error), 1);
  EXPECT_NE(error.find("could not confirm"), error.npos);
}

TEST_F(MonodRuntimeTest, BadConfigOrMarketFailureNeverSendsOrders) {
  std::ofstream(root / "bad.conf") << "strategy=strategy.conf\npassword=secret";
  EXPECT_FALSE(ReadMonodConfig((root / "bad.conf").string(), config, error));
  EXPECT_NE(error.find("password"), error.npos);
  EXPECT_EQ(error.find("secret"), error.npos);
  Counter counter;
  Quotes quotes(counter);
  quotes.broken = true;
  EXPECT_EQ(RunMonod(config, quotes, counter, [] { return false; }, error), 1);
  EXPECT_TRUE(counter.orders.empty());
}

}  // namespace
}  // namespace yquant::runtime
