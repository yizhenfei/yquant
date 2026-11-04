#include "yquant/runtime/monod.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
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
  bool auto_fill = true;
  std::function<void()> after_run;
  std::function<void(const AdapterCancelOrderRequest&)> on_cancel;
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
    if (on_cancel) on_cancel(request);
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
      baseline.order_terms.emplace(81, std::pair{terms,terms});
      sink->OnAccountRecovery(sync, std::move(baseline));
      sink->OnAccountSyncReport({sync, AccountSyncResult::kComplete});
      started = true;
    }
    sim.Run();
    if (auto_fill && runs == 9 && !orders.empty()) {
      const auto order = orders.back();
      if (sim.Fill({order.order_id, {"TEST","day","fill"}, order.price, 1})) ++fills;
      sim.Run();
    }
    if (after_run) after_run();
  }
};

// Drops an early valid quote before trade readiness, then supplies stable and
// changed prices on distinct scheduling rounds.
class Quotes final : public market::SimNowQuoteSource {
 public:
  explicit Quotes(Counter& counter) : counter(counter) {}
  Counter& counter;
  int last = -1;
  bool broken = false, multi = false;
  int ready_after = 0;
  // SimNowQuoteSource implementation.
  bool failed() const override { return broken; }
  bool ready() const override { return !broken && counter.started && counter.runs >= ready_after; }
  // QuoteSource implementation.
  bool TryRead(model::Quote& q) override {
    if (last == counter.runs) return false;
    last = counter.runs;
    q = {42, counter.runs < 4 ? 50.0 : counter.runs < 6 ? 100.0 : 101.0,
         103,1,1,static_cast<std::uint64_t>(counter.runs)};
    if (multi && counter.runs % 2) q.instrument_id = 81;
    return true;
  }
};

class MonodRuntimeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char path[] = "/tmp/yquant-runtime-XXXXXX";
    ASSERT_NE(mkdtemp(path), nullptr);
    root = path;
    std::ofstream(root / "strategy.conf") << "library=" << MAKER_PLUGIN
        << "\nstrategy_id=11\naccount_id=7\nparam.instrument_id=42\nparam.quantity=1\n";
    std::ofstream(root / "runtime.conf") << "strategy=strategy.conf\n"
        "market_front=tcp://fixture:1\ntrading_front=tcp://fixture:2\n"
        "instrument.42=TEST contract\ncaller_service_id=10\n"
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
  EXPECT_NE(out.find("monod_started services=1 strategies=1 account=7"), out.npos);
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

// Uses the production parser and loader with independently observable plugins.
class MultiRuntimeTest : public MonodRuntimeTest {
 protected:
  void SetUp() override {
    MonodRuntimeTest::SetUp();
    WriteStrategy("a", 11, 42);
    WriteStrategy("b", 12, 42);
    WriteStrategy("c", 11, 81);
    deployment = "market_front=tcp://fixture:1\ntrading_front=tcp://fixture:2\n"
        "startup_seconds=1\nshutdown_seconds=1\nrun_seconds=1\n"
        "strategy.10.11=a.conf\nstrategy.10.12=b.conf\nstrategy.20.11=c.conf\n"
        "instrument.42=TEST contract today\ninstrument.81=TEST other yesterday\n";
    ASSERT_TRUE(Read(deployment)) << error;
  }
  void WriteStrategy(const std::string& name, int id, int instrument,
                     const std::string& library = FIXED_PLUGIN, int account = 7) {
    std::ofstream file(root / (name + ".conf"));
    file << "library=" << library
        << "\nstrategy_id=" << id << "\naccount_id=" << account
        << "\nparam.instrument_id=" << instrument << "\nparam.quantity=2\n";
    if (library != MAKER_PLUGIN) {
      file << "param.price=100\nparam.marker=" << (root / name).string() << "\n";
    }
  }
  bool Read(const std::string& contents) {
    std::ofstream(root / "multi.conf") << contents;
    return ReadMonodConfig((root / "multi.conf").string(), config, error);
  }
  std::string Reports(const std::string& name) {
    std::ifstream file(root / (name + ".orders"));
    return {std::istreambuf_iterator<char>(file), {}};
  }
  std::string deployment;
};

TEST_F(MultiRuntimeTest, GroupsShareAccountWithIndependentCallersAndCompleteShutdown) {
  Counter counter;
  counter.auto_fill = false;
  Quotes quotes(counter);
  quotes.multi = true;
  quotes.ready_after = 6;
  int late_reports = 0;
  counter.after_run = [&] {
    if (counter.runs < 6) EXPECT_TRUE(counter.orders.empty());
    for (const auto& cancel : counter.cancels) {
      // After plugin destruction, repeat terminal and late order callbacks.
      ASSERT_TRUE(std::filesystem::exists(root / "a"));
      ASSERT_TRUE(std::filesystem::exists(root / "b"));
      ASSERT_TRUE(std::filesystem::exists(root / "c"));
      counter.sim.EmitCancel({cancel.order_id, cancel.service_cancel_id, CancelResult::kCanceled, 0});
      counter.sim.Emit({cancel.order_id, AdapterOrderState::kAccepted, 0});
      ++late_reports;
    }
  };
  std::string a, b, c;
  ::testing::internal::CaptureStdout();
  const int rc = RunMonod(config, quotes, counter, [&] {
    if (counter.runs < 9) return false;
    a = Reports("a"); b = Reports("b"); c = Reports("c");
    return true;
  }, error);
  const auto out = ::testing::internal::GetCapturedStdout();
  ASSERT_EQ(rc, 0) << error;
  ASSERT_EQ(counter.orders.size(), 3);
  EXPECT_EQ(counter.orders[0].order_id, (OrderId{10,1}));
  EXPECT_EQ(counter.orders[1].order_id, (OrderId{10,2}));
  EXPECT_EQ(counter.orders[2].order_id, (OrderId{20,1}));
  EXPECT_EQ(counter.orders[0].strategy_id, 11);
  EXPECT_EQ(counter.orders[1].strategy_id, 12);
  EXPECT_EQ(counter.orders[2].strategy_id, 11);
  EXPECT_EQ(counter.orders[2].instrument_id, 81);
  for (const auto& order : counter.orders) EXPECT_EQ(order.account_id, 7);
  EXPECT_EQ(a, "11 10 1\n"); EXPECT_EQ(b, "12 10 2\n"); EXPECT_EQ(c, "11 20 1\n");
  EXPECT_EQ(Reports("a"), a); EXPECT_EQ(Reports("b"), b); EXPECT_EQ(Reports("c"), c);
  EXPECT_EQ(counter.cancels.size(), 3);
  EXPECT_EQ(late_reports, 3);
  for (const std::string owner : {"caller=10 strategy=11 sequence=1",
                                  "caller=10 strategy=12 sequence=2",
                                  "caller=20 strategy=11 sequence=1"}) {
    const auto at = out.find("monod_canceled " + owner);
    EXPECT_NE(at, out.npos);
    if (at != out.npos) EXPECT_EQ(out.find("monod_canceled " + owner, at + 1), out.npos);
  }
  EXPECT_NE(out.find("monod_started services=2 strategies=3 account=7"), out.npos);
  EXPECT_NE(out.find("monod_stopped canceled=3 remaining=0"), out.npos);
}

TEST_F(MultiRuntimeTest, ShutdownCancelsAllInstrumentsOwnedByEachInstance) {
  std::ifstream input(root / "a.conf");
  std::string settings{std::istreambuf_iterator<char>(input), {}};
  const auto start = settings.find("param.instrument_id=42");
  ASSERT_NE(start, settings.npos);
  settings.replace(start, std::string("param.instrument_id=42").size(),
                   "param.instrument_id=42 81");
  std::ofstream(root / "a.conf") << settings;
  ASSERT_TRUE(Read(deployment)) << error;
  Counter counter;
  counter.auto_fill = false;
  Quotes quotes(counter);
  quotes.multi = true;
  EXPECT_EQ(RunMonod(config, quotes, counter, [&] { return counter.runs >= 9; }, error), 0) << error;
  ASSERT_EQ(counter.orders.size(), 4);
  EXPECT_EQ(counter.cancels.size(), 4);
  std::set<std::uint32_t> instruments;
  for (const auto& order : counter.orders) {
    if (order.order_id.caller_service_id == 10 && order.strategy_id == 11) {
      instruments.insert(order.instrument_id);
    }
  }
  EXPECT_EQ(instruments, (std::set<std::uint32_t>{42, 81}));
}

TEST_F(MultiRuntimeTest, PluginFailureRollsBackAllPreviouslyCreatedGroups) {
  WriteStrategy("c", 11, 81, FAIL_PLUGIN);
  ASSERT_TRUE(Read(deployment)) << error;
  Counter counter;
  Quotes quotes(counter);
  EXPECT_EQ(RunMonod(config, quotes, counter, [] { return false; }, error), 1);
  EXPECT_NE(error.find("creation failed"), error.npos);
  for (const auto* name : {"a", "b", "c"}) {
    EXPECT_TRUE(std::filesystem::exists(root / name));
    EXPECT_TRUE(Reports(name).empty());
  }
  EXPECT_TRUE(counter.orders.empty());
  EXPECT_EQ(counter.runs, 0);
}

TEST_F(MultiRuntimeTest, RejectsAmbiguousIdentityMappingsAndMixedAccounts) {
  for (const auto& extra : {
      "strategy.010.11=a.conf\n", "strategy.10.13=a.conf\n",
      "instrument.042=TEST second\n", "instrument.82=OTHER contract\n",
      "caller_service_id=10\n", "instrument.82=TEST third today extra\n"}) {
    EXPECT_FALSE(Read(deployment + extra)) << extra;
    EXPECT_EQ(config.services.size(), 2);  // Failed reads preserve the prior config.
  }
  WriteStrategy("c", 11, 81, FIXED_PLUGIN, 8);
  EXPECT_FALSE(Read(deployment));
  WriteStrategy("c", 11, 99);
  ASSERT_TRUE(Read(deployment));
  Counter counter;
  Quotes quotes(counter);
  EXPECT_EQ(RunMonod(config, quotes, counter, [] { return false; }, error), 1);
  EXPECT_NE(error.find("no deployment instrument mapping"), error.npos);
  EXPECT_TRUE(counter.orders.empty());
  EXPECT_EQ(counter.runs, 0);
}

TEST_F(MultiRuntimeTest, ShutdownCollectsOtherGroupsAfterOneRejection) {
  Counter counter;
  counter.auto_fill = false;
  Quotes quotes(counter);
  quotes.multi = true;
  counter.on_cancel = [&](const auto& cancel) {
    if (cancel.order_id == OrderId{10,1}) {
      counter.sim.SetNextCancel({SendResult::kSent, CancelResult::kRejected});
    } else {
      // Delay successes to a later Run so one rejection cannot end collection.
      counter.sim.SetNextCancel({SendResult::kSent, std::nullopt});
    }
  };
  counter.after_run = [&] {
    if (counter.runs == 12) {
      for (const auto& cancel : counter.cancels) {
        if (cancel.order_id != OrderId{10,1}) {
          counter.sim.EmitCancel({cancel.order_id, cancel.service_cancel_id, CancelResult::kCanceled, 0});
        }
      }
    }
  };
  ::testing::internal::CaptureStderr();
  const int rc = RunMonod(config, quotes, counter, [&] { return counter.runs >= 9; }, error);
  const auto diagnostics = ::testing::internal::GetCapturedStderr();
  EXPECT_EQ(rc, 1);
  EXPECT_EQ(counter.runs, 12);
  EXPECT_NE(error.find("remaining=1"), error.npos);
  EXPECT_NE(diagnostics.find("caller=10 strategy=11 sequence=1"), diagnostics.npos);
  EXPECT_EQ(diagnostics.find("caller=20"), diagnostics.npos);
  EXPECT_EQ(diagnostics.find("strategy=12"), diagnostics.npos);
}

TEST_F(MultiRuntimeTest, ShutdownWaitsForLatePricedFillsFromEveryInstance) {
  Counter counter;
  counter.auto_fill = false;
  Quotes quotes(counter);
  quotes.multi = true;
  counter.on_cancel = [&](const auto&) {
    counter.sim.SetNextCancel({SendResult::kSent, std::nullopt});
  };
  counter.after_run = [&] {
    if (counter.runs == 10) {
      for (const auto& cancel : counter.cancels) {
        counter.sim.EmitCancel({cancel.order_id, cancel.service_cancel_id, CancelResult::kCanceled, 1});
      }
    }
    if (counter.runs == 11) {
      for (const auto& order : counter.orders) {
        TradeInfo fill{order.order_id, {"TEST", "day", std::to_string(order.order_id.caller_service_id)
            + "-" + std::to_string(order.order_id.sequence)}, 100, 1};
        counter.sim.EmitTrade(fill);
        counter.sim.EmitTrade(fill);
      }
    }
  };
  ::testing::internal::CaptureStdout();
  const int rc = RunMonod(config, quotes, counter, [&] { return counter.runs >= 9; }, error);
  const auto out = ::testing::internal::GetCapturedStdout();
  EXPECT_EQ(rc, 0) << error;
  EXPECT_EQ(counter.runs, 11);
  EXPECT_NE(out.find("remaining=0 commission=3"), out.npos);
  EXPECT_EQ(Reports("a"), "11 10 1\n");
  EXPECT_EQ(Reports("b"), "12 10 2\n");
  EXPECT_EQ(Reports("c"), "11 20 1\n");
}

TEST_F(MultiRuntimeTest, TimeoutReportsEveryPendingOrderAndCancellation) {
  Counter counter;
  counter.auto_fill = false;
  Quotes quotes(counter);
  quotes.multi = true;
  counter.on_cancel = [&](const auto&) {
    counter.sim.SetNextCancel({SendResult::kSent, std::nullopt});
  };
  ::testing::internal::CaptureStderr();
  const int rc = RunMonod(config, quotes, counter, [&] { return counter.runs >= 9; }, error);
  const auto diagnostics = ::testing::internal::GetCapturedStderr();
  EXPECT_EQ(rc, 1);
  EXPECT_NE(error.find("remaining=3"), error.npos);
  for (const std::string id : {"caller=10 strategy=11 sequence=1",
                              "caller=10 strategy=12 sequence=2",
                              "caller=20 strategy=11 sequence=1"}) {
    EXPECT_NE(diagnostics.find("monod_unconfirmed " + id), diagnostics.npos);
    EXPECT_NE(diagnostics.find("monod_cancel_unconfirmed " + id + " request=1"), diagnostics.npos);
  }
}

TEST_F(MultiRuntimeTest, MakersReuseCancelIdsWithoutLosingInstanceOwnership) {
  WriteStrategy("a", 11, 42, MAKER_PLUGIN);
  WriteStrategy("b", 12, 42, MAKER_PLUGIN);
  WriteStrategy("c", 11, 81, MAKER_PLUGIN);
  ASSERT_TRUE(Read(deployment)) << error;
  Counter counter;
  counter.reject_first_cancel = true;
  Quotes quotes(counter);
  quotes.multi = true;
  EXPECT_EQ(RunMonod(config, quotes, counter, [&] { return counter.runs > 14; }, error), 0) << error;
  std::set<std::pair<std::uint64_t, std::uint64_t>> ids;
  std::set<std::pair<std::uint64_t, std::uint64_t>> instances;
  for (const auto& order : counter.orders) {
    EXPECT_TRUE(ids.emplace(order.order_id.caller_service_id, order.order_id.sequence).second);
    instances.emplace(order.order_id.caller_service_id, order.strategy_id);
  }
  EXPECT_EQ(instances.size(), 3);
  EXPECT_EQ(counter.fills, 1);
  EXPECT_GE(counter.cancels.size(), 6);
}

}  // namespace
}  // namespace yquant::runtime
