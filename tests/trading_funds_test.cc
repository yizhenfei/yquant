#include "yquant/trading/trading_client_factory.h"

#include <limits>
#include <memory>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/model/currency.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {
namespace {

const FundsInfo kInitial{model::Currency("CNY"), 1000, 800, 200, 0};

// Verifies initialization, retries and fixed baselines through public clients.
class FundsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(service.BindAccount(7, first));
    ASSERT_TRUE(service.BindAccount(8, second));
    TradingServiceRegistry registry;
    ASSERT_TRUE(registry.Register(3, service));
    client = TradingClientFactory(registry).Create(3);
    observer = TradingClientFactory(registry).Create(3);
  }
  void Complete(SimulatedTradingAdapter& adapter, std::uint64_t id,
                const FundsInfo& funds) {
    adapter.QueueFunds({id, FundsRefreshStarted{}});
    adapter.QueueFunds({id, funds});
    adapter.QueuePositions({id, PositionsRefreshStarted{}});
    adapter.QueuePositions({id, PositionsRefreshCompleted{0}});
    adapter.QueueAccountSync({id, AccountSyncResult::kComplete});
    service.Run();
  }
  void Unavailable(std::uint64_t account = 7) {
    FundsSnapshot sentinel{kInitial, 99, 3};
    auto output = sentinel;
    EXPECT_FALSE(client->QueryFunds({account}, output));
    EXPECT_EQ(output, sentinel);
  }
  SimulatedTradingAdapter first, second;
  TradingService service;
  std::unique_ptr<TradingClient> client, observer;
};

TEST_F(FundsTest, UnknownAndPartialAccountLeaveOutputUntouched) {
  Unavailable();
  Unavailable(99);
  first.QueueFunds({1, FundsRefreshStarted{}});
  first.QueueFunds({1, kInitial});
  service.Run();
  Unavailable();
  first.QueuePositions({1, PositionsRefreshStarted{}});
  first.QueuePositions({1, PositionsRefreshCompleted{0}});
  service.Run();
  Unavailable();
  first.QueueAccountSync({1, AccountSyncResult::kComplete});
  service.Run();
  FundsSnapshot output;
  ASSERT_TRUE(client->QueryFunds({7}, output));
  EXPECT_EQ(output, (FundsSnapshot{kInitial, 1}));
}

TEST_F(FundsTest, ZeroAndSignedAmountsArePreservedAndSharedPerAccount) {
  const FundsInfo zero{model::Currency("USD"), 0, 0, 0, 0};
  Complete(first, 1, zero);
  Complete(second, 1, {model::Currency("CNY"), -10, -30, 20, 0});
  FundsSnapshot first_value, second_value, shared;
  ASSERT_TRUE(client->QueryFunds({7}, first_value));
  ASSERT_TRUE(client->QueryFunds({8}, second_value));
  ASSERT_TRUE(observer->QueryFunds({7}, shared));
  EXPECT_EQ(first_value, shared);
  EXPECT_EQ(first_value.funds, zero);
  EXPECT_DOUBLE_EQ(second_value.funds.available, -30);
}

TEST_F(FundsTest, FailureNeedsExplicitRetryAndIgnoresOldResults) {
  first.QueueFunds({1, FundsRefreshStarted{}});
  first.QueueFunds({1, FundsRefreshFailed{}});
  service.Run();
  Unavailable();
  service.Run();
  EXPECT_EQ(first.sync_requests(), 1U);
  ASSERT_TRUE(service.RetryAccountSync(7));
  Complete(first, 1, kInitial);
  Unavailable();
  EXPECT_EQ(first.sync_requests(), 2U);
  Complete(first, 2, kInitial);
  FundsSnapshot output;
  ASSERT_TRUE(client->QueryFunds({7}, output));
  EXPECT_EQ(output, (FundsSnapshot{kInitial, 2}));
}

TEST_F(FundsTest, RuntimeSnapshotsFailuresAndCompletionsCannotReplaceBaseline) {
  Complete(first, 1, kInitial);
  first.QueueFunds({1, FundsRefreshFailed{}});
  first.QueueAccountSync({1, AccountSyncResult::kFailed});
  Complete(first, 2, {model::Currency("USD"), 0, 0, 0, 0});
  FundsSnapshot output;
  ASSERT_TRUE(client->QueryFunds({7}, output));
  EXPECT_EQ(output, (FundsSnapshot{kInitial, 1}));
  EXPECT_FALSE(service.RetryAccountSync(7));
  EXPECT_EQ(first.sync_requests(), 1U);
}

TEST_F(FundsTest, UnrequestedAndDuplicateEventsCannotAdvanceInitialization) {
  Complete(first, 99, kInitial);
  Complete(first, 0, kInitial);
  Unavailable();
  first.QueueFunds({1, FundsRefreshStarted{}});
  first.QueueFunds({1, FundsRefreshStarted{}});
  Complete(first, 1, kInitial);
  EXPECT_TRUE(service.IsAccountReady(7));
}

TEST_F(FundsTest, InvalidAmountsAndCurrenciesFailWithoutPublishing) {
  std::vector<FundsInfo> invalid;
  for (auto field : {&FundsInfo::balance, &FundsInfo::available,
                     &FundsInfo::margin, &FundsInfo::frozen_margin}) {
    for (double value : {std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
      auto funds = kInitial;
      funds.*field = value;
      invalid.push_back(funds);
    }
  }
  auto bad_currency = kInitial;
  bad_currency.currency = model::Currency("cny");
  invalid.push_back(bad_currency);
  std::uint64_t id = 1;
  for (const auto& funds : invalid) {
    Complete(first, id++, funds);
    Unavailable();
    ASSERT_TRUE(service.RetryAccountSync(7));
  }
  Complete(first, id, kInitial);
  EXPECT_TRUE(service.IsAccountReady(7));
}

TEST_F(FundsTest, QueueOwnsValuesAndAccountsProgressIndependently) {
  first.QueueFunds({1, FundsRefreshStarted{}});
  AdapterFundsReport report{1, kInitial};
  first.QueueFunds(report);
  report.event = FundsRefreshFailed{};
  Complete(second, 1, kInitial);
  Unavailable();
  EXPECT_TRUE(service.IsAccountReady(8));
  first.QueuePositions({1, PositionsRefreshStarted{}});
  first.QueuePositions({1, PositionsRefreshCompleted{0}});
  first.QueueAccountSync({1, AccountSyncResult::kComplete});
  service.Run();
  FundsSnapshot output;
  ASSERT_TRUE(client->QueryFunds({7}, output));
  EXPECT_EQ(output.funds, kInitial);
}

}  // namespace
}  // namespace yquant::trading
