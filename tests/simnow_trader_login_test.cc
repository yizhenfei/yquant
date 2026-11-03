#include "yquant/trading/simnow_trading_adapter.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "simnow_trader_session.h"
#include "trader_login.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {
namespace {
using namespace std::chrono_literals;

const SimNowTraderConfig kConfig{
    "tcp://fixture:1", "test", "test", "test", "test", "test"};

// Records native request fields and injects responses through the real SPI.
class Probe final : public CThostFtdcTraderApi {
 public:
  // CThostFtdcTraderApi implementation.
  void RegisterSpi(CThostFtdcTraderSpi* callbacks) override { spi = callbacks; }
  void RegisterFront(char* address) override { front = address; }
  void SubscribePrivateTopic(THOST_TE_RESUME_TYPE mode) override {
    private_quick = mode == THOST_TERT_QUICK;
  }
  void SubscribePublicTopic(THOST_TE_RESUME_TYPE mode) override {
    public_quick = mode == THOST_TERT_QUICK;
  }
  void Init() override {
    ASSERT_TRUE(spi && !front.empty() && private_quick && public_quick);
    if (automatic) spi->OnFrontConnected();
  }
  void Release() override {
    EXPECT_EQ(spi, nullptr);
    delete this;
  }
  int ReqAuthenticate(CThostFtdcReqAuthenticateField* r, int id) override {
    auth = *r;
    auth_id = id;
    caller = std::this_thread::get_id();
    ++auth_calls;
    if (automatic) AuthReply(id);
    return auth_rc;
  }
#ifdef YQUANT_CTP_MAC_LOGIN
  int ReqUserLogin(CThostFtdcReqUserLoginField* r, int id, int length,
                   TThostFtdcClientSystemInfoType info) override {
    system_info.assign(info, length);
#else
  int ReqUserLogin(CThostFtdcReqUserLoginField* r, int id) override {
#endif
    login = *r;
    login_id = id;
    caller = std::this_thread::get_id();
    ++login_calls;
    if (automatic) LoginReply(id);
    return login_rc;
  }
  int ReqQryTradingAccount(CThostFtdcQryTradingAccountField* r, int id) override {
    EXPECT_STREQ(r->BrokerID, "test");
    EXPECT_STREQ(r->InvestorID, "test");
    funds_ids.push_back(id);
    if (automatic) {
      auto row = Balance();
      spi->OnRspQryTradingAccount(&row, nullptr, id, true);
    }
    return funds_rc;
  }
  int ReqQryInvestorPosition(CThostFtdcQryInvestorPositionField* r, int id) override {
    EXPECT_STREQ(r->InvestorID, "test");
    EXPECT_STREQ(r->BrokerID, "test");
    EXPECT_STREQ(r->InstrumentID, "");
    position_ids.push_back(id);
    if (automatic) spi->OnRspQryInvestorPosition(nullptr, nullptr, id, true);
    return 0;
  }
#include <unused_trader_calls.inc>

  static CThostFtdcTradingAccountField Balance() {
    CThostFtdcTradingAccountField r{};
    std::snprintf(r.AccountID, sizeof(r.AccountID), "test");
    std::snprintf(r.BrokerID, sizeof(r.BrokerID), "test");
    std::snprintf(r.CurrencyID, sizeof(r.CurrencyID), "CNY");
    std::snprintf(r.TradingDay, sizeof(r.TradingDay), "20261103");
    r.Balance = 105;
    r.Available = 59;
    r.FrozenMargin = 20;
    r.FrozenCommission = 1;
    r.CurrMargin = 25;
    return r;
  }
  std::vector<int> funds_ids, position_ids;
  int funds_rc = 0;

  void AuthReply(int id, int code = 0, bool last = true) {
    CThostFtdcRspAuthenticateField r{};
    std::snprintf(r.BrokerID, sizeof(r.BrokerID), "%s", auth.BrokerID);
    std::snprintf(r.UserID, sizeof(r.UserID), "%s", auth.UserID);
    CThostFtdcRspInfoField error{};
    error.ErrorID = code;
    spi->OnRspAuthenticate(&r, &error, id, last);
  }
  void LoginReply(int id, int code = 0, bool last = true) {
    CThostFtdcRspUserLoginField r{};
    std::snprintf(r.BrokerID, sizeof(r.BrokerID), "%s", login.BrokerID);
    std::snprintf(r.UserID, sizeof(r.UserID), "%s", login.UserID);
    std::snprintf(r.TradingDay, sizeof(r.TradingDay), "20261103");
    std::snprintf(r.MaxOrderRef, sizeof(r.MaxOrderRef), "  123");
    r.FrontID = 3;
    r.SessionID = 7;
    CThostFtdcRspInfoField error{};
    error.ErrorID = code;
    spi->OnRspUserLogin(&r, &error, id, last);
  }
  CThostFtdcTraderSpi* spi = nullptr;
  CThostFtdcReqAuthenticateField auth{};
  CThostFtdcReqUserLoginField login{};
  std::string front, system_info;
  std::thread::id caller;
  int auth_id = 0, login_id = 0, auth_calls = 0, login_calls = 0;
  int auth_rc = 0, login_rc = 0;
  bool automatic = false, private_quick = false, public_quick = false;
};

// Exposes the SDK object only while the owning session is alive.
auto OpenProbe(const SimNowTraderConfig& config, Probe*& probe) {
  return internal::StartTraderSession(config, [&] {
    probe = new Probe;
    return probe;
  });
}

class TraderLoginTest : public ::testing::Test {
 protected:
  void SetUp() override {
    session = OpenProbe(kConfig, probe);
    ASSERT_TRUE(session);
  }
  void Connect() {
    probe->spi->OnFrontConnected();
    session->Run();
  }
  void Authenticate() {
    Connect();
    probe->AuthReply(probe->auth_id);
    session->Run();
  }
  Probe* probe = nullptr;
  std::unique_ptr<SimNowTradingAdapter> session;
};

TEST_F(TraderLoginTest, BusinessThreadSendsRequestsAndPublishesSession) {
  std::thread([&] { probe->spi->OnFrontConnected(); }).join();
  EXPECT_EQ(probe->auth_calls, 0);
  session->Run();
  EXPECT_EQ(probe->caller, std::this_thread::get_id());
  EXPECT_EQ(probe->front, kConfig.front);
  EXPECT_STREQ(probe->auth.AppID, "test");
  EXPECT_STREQ(probe->auth.AuthCode, "test");
  EXPECT_EQ(session->login_status().phase, LoginPhase::kAuthenticating);
  std::thread([&] { probe->AuthReply(probe->auth_id); }).join();
  EXPECT_EQ(probe->login_calls, 0);
  session->Run();
  EXPECT_EQ(probe->caller, std::this_thread::get_id());
  EXPECT_STREQ(probe->login.BrokerID, "test");
  EXPECT_STREQ(probe->login.UserID, "test");
  EXPECT_STREQ(probe->login.Password, "test");
  EXPECT_NE(probe->auth_id, probe->login_id);
  std::thread([&] { probe->LoginReply(probe->login_id); }).join();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kLoggingIn);
  session->Run();
  const auto& s = session->login_status();
  EXPECT_EQ(s.phase, LoginPhase::kLoggedIn);
  EXPECT_EQ(s.trading_day, "20261103");
  EXPECT_EQ(s.max_order_ref, "  123");
  EXPECT_EQ(s.front_id, 3);
  EXPECT_EQ(s.session_id, 7);
}

TEST_F(TraderLoginTest, AuthRejectionNeverAttemptsLogin) {
  Connect();
  probe->AuthReply(probe->auth_id, 63);
  session->Run();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kFailed);
  EXPECT_EQ(session->login_status().code, 63);
  EXPECT_EQ(probe->login_calls, 0);
}

TEST_F(TraderLoginTest, LoginRejectionPreservesCode) {
  Authenticate();
  probe->LoginReply(probe->login_id, 3);
  session->Run();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kFailed);
  EXPECT_EQ(session->login_status().code, 3);
}

TEST_F(TraderLoginTest, ImmediateRequestFailureIsNotRetried) {
  probe->auth_rc = -2;
  Connect();
  for (int i = 0; i < 10; ++i) session->Run();
  EXPECT_EQ(probe->auth_calls, 1);
  EXPECT_EQ(session->login_status().phase, LoginPhase::kFailed);
  EXPECT_EQ(session->login_status().code, -2);
}

TEST_F(TraderLoginTest, LoginRequestFailureNeverBecomesSuccess) {
  probe->login_rc = -3;
  Authenticate();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kFailed);
  EXPECT_STREQ(session->login_status().reason, "login_request");
  probe->LoginReply(probe->login_id);
  session->Run();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kFailed);
}

TEST_F(TraderLoginTest, GenericErrorMatchesOnlyTheOutstandingRequest) {
  Authenticate();
  CThostFtdcRspInfoField error{};
  error.ErrorID = 7;
  probe->spi->OnRspError(&error, probe->auth_id, true);
  session->Run();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kLoggingIn);
  probe->spi->OnRspError(&error, probe->login_id, true);
  session->Run();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kFailed);
  EXPECT_EQ(session->login_status().code, 7);
}

TEST_F(TraderLoginTest, DifferentAccountCannotCompleteLogin) {
  Authenticate();
  CThostFtdcRspUserLoginField other{};
  std::snprintf(other.TradingDay, sizeof(other.TradingDay), "20261103");
  probe->spi->OnRspUserLogin(&other, nullptr, probe->login_id, true);
  session->Run();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kFailed);
}

TEST_F(TraderLoginTest, NonfinalAndUnrelatedResponsesCannotAdvanceLogin) {
  Connect();
  probe->AuthReply(probe->auth_id, 0, false);
  probe->AuthReply(probe->auth_id + 1);
  session->Run();
  session->Run();
  EXPECT_EQ(probe->login_calls, 0);
  probe->AuthReply(probe->auth_id);
  session->Run();
  probe->LoginReply(probe->login_id, 0, false);
  session->Run();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kLoggingIn);
  probe->spi->OnRspUserLogin(nullptr, nullptr, probe->login_id, true);
  session->Run();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kFailed);
}

TEST_F(TraderLoginTest, DisconnectInvalidatesOldResponsesAndRenewsRequestIds) {
  Authenticate();
  const int old = probe->login_id;
  probe->LoginReply(old);
  probe->spi->OnFrontDisconnected(1001);
  probe->spi->OnFrontConnected();
  session->Run();  // Discard login from the disconnected generation.
  EXPECT_NE(session->login_status().phase, LoginPhase::kLoggedIn);
  session->Run();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kDisconnected);
  EXPECT_EQ(session->login_status().code, 1001);
  session->Run();
  EXPECT_EQ(probe->auth_calls, 2);
  probe->AuthReply(probe->auth_id);
  session->Run();
  probe->LoginReply(old);
  session->Run();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kLoggingIn);
  probe->LoginReply(probe->login_id);
  session->Run();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kLoggedIn);
  probe->spi->OnFrontDisconnected(1002);
  session->Run();
  EXPECT_TRUE(session->login_status().trading_day.empty());
}

TEST_F(TraderLoginTest, QueueOverflowNeverPublishesAPartialSession) {
  for (int i = 0; i < 33; ++i) probe->spi->OnFrontConnected();
  session->Run();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kFailed);
  EXPECT_EQ(probe->auth_calls, 0);
}

TEST(TraderLogin, TimeoutRequiresANewSession) {
  auto config = kConfig;
  config.timeout = 5ms;
  auto session = internal::StartTraderSession(config, [] { return new Probe; });
  ASSERT_TRUE(session);
  std::this_thread::sleep_for(10ms);
  session->Run();
  EXPECT_EQ(session->login_status().phase, LoginPhase::kFailed);
  EXPECT_STREQ(session->login_status().reason, "connection_timeout");
}

TEST(TraderLogin, DeadlineIncludesWaitingForLoginResponse) {
  auto config = kConfig;
  config.timeout = 100ms;
  Probe* probe = nullptr;
  auto session = OpenProbe(config, probe);
  ASSERT_TRUE(session);
  probe->spi->OnFrontConnected();
  session->Run();
  probe->AuthReply(probe->auth_id);
  session->Run();
  ASSERT_EQ(session->login_status().phase, LoginPhase::kLoggingIn);
  std::this_thread::sleep_for(110ms);
  session->Run();
  EXPECT_STREQ(session->login_status().reason, "login_timeout");
}
#ifdef YQUANT_CTP_MAC_LOGIN
TEST(TraderLogin, MacTerminalPayloadIsCopiedWithItsExactByteLength) {
  char bytes[] = {'a', 0, 'b'};
  auto config = kConfig;
  config.system_info = {bytes, sizeof(bytes)};
  Probe* probe = nullptr;
  auto session = OpenProbe(config, probe);
  ASSERT_TRUE(session);
  bytes[0] = 'x';
  probe->spi->OnFrontConnected();
  session->Run();
  probe->AuthReply(probe->auth_id);
  session->Run();
  EXPECT_EQ(probe->system_info, std::string("a\0b", 3));
}
#endif

TEST(TraderLogin, InvalidOrAnonymousConfigurationIsRejected) {
  auto unexpected_create = []() -> CThostFtdcTraderApi* {
    ADD_FAILURE() << "Invalid input reached SDK creation";
    return nullptr;
  };
  for (const auto member : {&SimNowTraderConfig::broker, &SimNowTraderConfig::user,
       &SimNowTraderConfig::password, &SimNowTraderConfig::app_id,
       &SimNowTraderConfig::auth_code}) {
    auto config = kConfig;
    config.*member = "";
    EXPECT_FALSE(internal::StartTraderSession(config, unexpected_create));
  }
  std::string too_long(256, 'x');
  auto config = kConfig;
  config.password = too_long;
  EXPECT_FALSE(internal::StartTraderSession(config, unexpected_create));
}

// Retains request failures for verification after the client call returns.
class Outcomes final : public TradingListener {
 public:
  // TradingListener implementation.
  void OnRequestFailure(const RequestFailure& f) override {
    failures.push_back(f.reason);
  }
  void OnOrderReport(const OrderReport&) override { ADD_FAILURE(); }
  void OnCancelReport(const CancelReport&) override { ADD_FAILURE(); }
  void OnTradeReport(const TradeReport&) override { ADD_FAILURE(); }
  std::vector<FailureReason> failures;
};

TEST_F(TraderLoginTest, AuthenticatedAccountStillRejectsOrdersBeforeSending) {
  Authenticate();
  probe->LoginReply(probe->login_id);
  session->Run();
  ASSERT_EQ(session->login_status().phase, LoginPhase::kLoggedIn);
  Outcomes outcomes;
  TradingService service;
  ASSERT_TRUE(service.BindAccount(7, *session));
  TradingServiceRegistry registry;
  ASSERT_TRUE(registry.Register(3, service));
  auto client = TradingClientFactory(registry).Create(3);
  ASSERT_TRUE(client);
  client->SubmitOrder({{{3, 1}, 7, 4, 42, Side::kBuy, Offset::kOpen, 100, 1}},
                      outcomes);
  EXPECT_TRUE(outcomes.failures.empty());
  service.Run();
  EXPECT_FALSE(service.IsAccountReady(7));
  EXPECT_EQ(outcomes.failures, std::vector{FailureReason::kAccountNotReady});
  OrderSnapshot order;
  EXPECT_FALSE(client->QueryOrder({{3, 1}, 7, 4}, order));
}

// Starts each snapshot test with an authenticated native SDK session.
class StartupSnapshotTest : public TraderLoginTest {
 protected:
  void SetUp() override {
    TraderLoginTest::SetUp();
    EXPECT_FALSE(session->StartSnapshot());
    Authenticate();
    probe->LoginReply(probe->login_id);
    session->Run();
  }
  void Start() {
    ASSERT_TRUE(session->StartSnapshot());
    ASSERT_FALSE(session->StartSnapshot());
    session->Run();
    ASSERT_FALSE(probe->funds_ids.empty());
  }
  void BeginPositions() {
    Start();
    auto funds = Probe::Balance();
    probe->spi->OnRspQryTradingAccount(&funds, nullptr, probe->funds_ids.back(), true);
    session->Run();
    ASSERT_EQ(session->snapshot_status().phase, SnapshotPhase::kPositions);
    EXPECT_EQ(session->snapshot(), nullptr);
    session->Run();
    ASSERT_EQ(probe->position_ids.size(), 1);
  }
  CThostFtdcInvestorPositionField Position(int i) {
    CThostFtdcInvestorPositionField r{};
    std::snprintf(r.BrokerID, sizeof(r.BrokerID), "test");
    std::snprintf(r.InvestorID, sizeof(r.InvestorID), "test");
    std::snprintf(r.TradingDay, sizeof(r.TradingDay), "20261103");
    std::snprintf(r.InstrumentID, sizeof(r.InstrumentID), "test%02d", i);
    std::snprintf(r.ExchangeID, sizeof(r.ExchangeID), "SHFE");
    r.PositionDate = THOST_FTDC_PSD_Today;
    r.HedgeFlag = THOST_FTDC_HF_Speculation;
    r.PosiDirection = THOST_FTDC_PD_Long;
    r.TodayPosition = r.Position = 4;
    r.ShortFrozen = 2;
    r.PositionCost = 400;
    r.UseMargin = 40;
    return r;
  }
};

TEST_F(StartupSnapshotTest, PublishesAllRowsAfterFinalResponseOnce) {
  BeginPositions();
  for (int i = 39; i >= 0; --i) {
    auto r = Position(i);
    probe->spi->OnRspQryInvestorPosition(&r, nullptr, probe->position_ids.back(), false);
  }
  for (int i = 0; i < 40; ++i) session->Run();
  EXPECT_FALSE(session->snapshot());
  probe->spi->OnRspQryInvestorPosition(nullptr, nullptr, probe->position_ids.back(), true);
  session->Run();
  auto* snapshot = session->snapshot();
  ASSERT_NE(snapshot, nullptr);
  ASSERT_EQ(snapshot->positions.size(), 40);
  const auto& r = snapshot->positions.front();
  EXPECT_EQ(r.symbol, "test00");
  EXPECT_EQ(r.quantity, 4);
  EXPECT_EQ(r.today, 4);
  EXPECT_EQ(r.short_frozen, 2);
  EXPECT_EQ(r.margin, 40);
  EXPECT_EQ(r.position_cost, 400);
  ASSERT_EQ(snapshot->balances.size(), 1);
  const auto& b = snapshot->balances.front();
  EXPECT_EQ(b.currency.code(), "CNY");
  EXPECT_EQ(b.available, 59);
  EXPECT_EQ(b.frozen_margin, 20);
  EXPECT_EQ(b.frozen_commission, 1);
  EXPECT_EQ(b.margin, 25);
  EXPECT_FALSE(session->StartSnapshot());
  probe->spi->OnRspQryInvestorPosition(nullptr, nullptr, probe->position_ids.back(), true);
  session->Run();
  EXPECT_EQ(probe->position_ids.size(), 1);
  probe->spi->OnFrontDisconnected(12);
  session->Run();
  EXPECT_FALSE(session->snapshot());
}

TEST_F(StartupSnapshotTest, EmptyFundsFailsAndRetryIgnoresOldRequestId) {
  Start();
  int old = probe->funds_ids.back();
  probe->spi->OnRspQryTradingAccount(nullptr, nullptr, old, true);
  session->Run();
  EXPECT_STREQ(session->snapshot_status().reason, "missing_funds");
  BeginPositions();
  probe->spi->OnRspError(nullptr, old, true);
  session->Run();
  EXPECT_EQ(session->snapshot_status().phase, SnapshotPhase::kPositions);
  probe->spi->OnRspQryInvestorPosition(nullptr, nullptr, probe->position_ids.back(), true);
  session->Run();
  ASSERT_TRUE(session->snapshot());
  EXPECT_TRUE(session->snapshot()->positions.empty());
}

TEST_F(StartupSnapshotTest, RejectsWrongAccountOrTradingDay) {
  for (bool change_day : {false, true}) {
    Start();
    auto r = Probe::Balance();
    (change_day ? r.TradingDay : r.AccountID)[0] = '9';
    probe->spi->OnRspQryTradingAccount(&r, nullptr, probe->funds_ids.back(), true);
    session->Run();
    EXPECT_STREQ(session->snapshot_status().reason, "funds_response");
    EXPECT_FALSE(session->snapshot());
    EXPECT_TRUE(probe->position_ids.empty());
  }
}

TEST_F(StartupSnapshotTest, DuplicateHoldingFailsEntireSnapshot) {
  BeginPositions();
  auto r = Position(1);
  for (bool last : {false, true}) {
    probe->spi->OnRspQryInvestorPosition(&r, nullptr, probe->position_ids.back(), last);
    session->Run();
  }
  EXPECT_FALSE(session->snapshot());
  EXPECT_STREQ(session->snapshot_status().reason, "duplicate_position");
}

TEST_F(StartupSnapshotTest, DisconnectInvalidatesQueuedFinalRow) {
  BeginPositions();
  probe->spi->OnRspQryInvestorPosition(nullptr, nullptr, probe->position_ids.back(), true);
  probe->spi->OnFrontDisconnected(3);
  session->Run();
  session->Run();
  EXPECT_FALSE(session->snapshot());
  EXPECT_STREQ(session->snapshot_status().reason, "snapshot_disconnected");
}

TEST_F(StartupSnapshotTest, RateLimitDelaysRetryAndUsesFreshId) {
  probe->funds_rc = -3;
  Start();
  probe->funds_rc = 0;
  session->Run();
  EXPECT_EQ(probe->funds_ids.size(), 1);
  std::this_thread::sleep_for(1010ms);
  session->Run();
  ASSERT_EQ(probe->funds_ids.size(), 2);
  EXPECT_GT(probe->funds_ids.back(), probe->funds_ids.front());
}

TEST(StartupSnapshot, MissingFinalResponseTimesOut) {
  auto config = kConfig;
  config.timeout = 50ms;
  Probe* probe = nullptr;
  auto session = OpenProbe(config, probe);
  probe->spi->OnFrontConnected();
  session->Run();
  probe->AuthReply(probe->auth_id);
  session->Run();
  probe->LoginReply(probe->login_id);
  session->Run();
  ASSERT_TRUE(session->StartSnapshot());
  session->Run();
  auto r = Probe::Balance();
  probe->spi->OnRspQryTradingAccount(&r, nullptr, probe->funds_ids.back(), false);
  session->Run();
  std::this_thread::sleep_for(60ms);
  session->Run();
  EXPECT_STREQ(session->snapshot_status().reason, "snapshot_timeout");
  EXPECT_FALSE(session->snapshot());
}

TEST(StartupSnapshot, MonodPublishesOnlyTheCompletedSummary) {
  const char* args[] = {"tcp://fixture:1", "2"};
  auto connect = [](const SimNowTraderConfig& config) {
    return internal::StartTraderSession(config, [] {
      auto* probe = new Probe;
      probe->automatic = true;
      return probe;
    });
  };
  ::testing::internal::CaptureStdout();
  const int result = monod::RunTraderLogin(2, args, true, connect);
  const auto out = ::testing::internal::GetCapturedStdout();
  EXPECT_EQ(result, 0);
  EXPECT_EQ(out, "startup_snapshot trading_day=20261103 currencies=1 "
                 "position_rows=0 account_ready=false\n");
}

TEST(TraderLogin, MonodProbeShowsLoginWithoutTradingReadiness) {
  const char* args[] = {"tcp://fixture:1", "2"};
  auto connect = [](const SimNowTraderConfig& config) {
    return internal::StartTraderSession(config, [] {
      auto* probe = new Probe;
      probe->automatic = true;
      return probe;
    });
  };
  ::testing::internal::CaptureStdout();
  const int result = monod::RunTraderLogin(2, args, false, connect);
  const auto out = ::testing::internal::GetCapturedStdout();
  EXPECT_EQ(result, 0);
  EXPECT_NE(out.find("trader_logged_in trading_day=20261103 account_ready=false"),
            std::string::npos);
}

}  // namespace
}  // namespace yquant::trading
