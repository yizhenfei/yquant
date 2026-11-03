#include "yquant/trading/simnow_trading_adapter.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "simnow_order_tag.h"
#include "simnow_trader_session.h"
#include "yquant/trading/order_id_generator.h"
#include "yquant/trading/internal/funds_calculation.h"
#include "yquant/trading/simulated_trading_adapter.h"
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
      auto row = account_balance;
      spi->OnRspQryTradingAccount(&row, nullptr, id, true);
    }
    return funds_rc;
  }
  int ReqQryInvestorPosition(CThostFtdcQryInvestorPositionField* r, int id) override {
    EXPECT_STREQ(r->InvestorID, "test");
    EXPECT_STREQ(r->BrokerID, "test");
    EXPECT_STREQ(r->InstrumentID, "");
    position_ids.push_back(id);
    if (automatic) {
      for (auto& row : position_rows) {
        spi->OnRspQryInvestorPosition(&row, nullptr, id, false);
      }
      spi->OnRspQryInvestorPosition(nullptr, nullptr, id, true);
    }
    return 0;
  }
  int ReqQryInvestorPositionDetail(CThostFtdcQryInvestorPositionDetailField* r,
                                   int id) override {
    EXPECT_STREQ(r->BrokerID, "test");
    EXPECT_STREQ(r->InvestorID, "test");
    detail_queries.emplace_back(r->InstrumentID);
    for (auto& row : details) {
      if (std::strcmp(r->InstrumentID, row.InstrumentID) == 0 &&
          std::strcmp(r->ExchangeID, row.ExchangeID) == 0) {
        spi->OnRspQryInvestorPositionDetail(&row, nullptr, id, false);
      }
    }
    spi->OnRspQryInvestorPositionDetail(nullptr, nullptr, id, true);
    return 0;
  }
  int ReqQryInstrument(CThostFtdcQryInstrumentField* r, int id) override {
    if (r->ExchangeID[0]) {
      position_contract_queries.emplace_back(r->InstrumentID);
      CThostFtdcInstrumentField instrument{};
      std::strcpy(instrument.InstrumentID, r->InstrumentID);
      std::strcpy(instrument.ExchangeID, r->ExchangeID);
      if (std::strcmp(r->InstrumentID, "TF2612") == 0) std::strcpy(instrument.ProductID, "TF");
      instrument.ProductClass = THOST_FTDC_PC_Futures;
      instrument.VolumeMultiple = multiplier;
      spi->OnRspQryInstrument(&instrument, nullptr, id, false);
      spi->OnRspQryInstrument(nullptr, nullptr, id, true);
      return 0;
    }
    EXPECT_STREQ(r->InstrumentID, "TF2612");
    CThostFtdcInstrumentField row{};
    std::snprintf(row.InstrumentID, sizeof(row.InstrumentID), "TF2612");
    std::snprintf(row.ProductID, sizeof(row.ProductID), "TF");
    std::snprintf(row.ExchangeID, sizeof(row.ExchangeID), "CFFEX");
    row.VolumeMultiple = 10;
    row.ProductClass = THOST_FTDC_PC_Futures;
    spi->OnRspQryInstrument(&row, nullptr, id, false);
    spi->OnRspQryInstrument(nullptr, nullptr, id, true);
    return 0;
  }
  int ReqQryProduct(CThostFtdcQryProductField* r, int id) override {
    EXPECT_STREQ(r->ProductID, "TF");
    EXPECT_STREQ(r->ExchangeID, "CFFEX");
    CThostFtdcProductField row{};
    std::snprintf(row.ProductID, sizeof(row.ProductID), "TF");
    std::snprintf(row.ExchangeID, sizeof(row.ExchangeID), "CFFEX");
    std::snprintf(row.TradeCurrencyID, sizeof(row.TradeCurrencyID), "CNY");
    spi->OnRspQryProduct(&row, nullptr, id, true);
    return 0;
  }
  int ReqQryInstrumentMarginRate(CThostFtdcQryInstrumentMarginRateField* r,
                                 int id) override {
    EXPECT_STREQ(r->InstrumentID, "TF2612");
    EXPECT_STREQ(r->ExchangeID, "CFFEX");
    EXPECT_STREQ(r->InvestorID, "test");
    EXPECT_STREQ(r->BrokerID, "test");
    EXPECT_EQ(r->HedgeFlag, THOST_FTDC_HF_Speculation);
    CThostFtdcInstrumentMarginRateField row{};
    std::snprintf(row.InstrumentID, sizeof(row.InstrumentID), "TF");
    std::snprintf(row.InvestorID, sizeof(row.InvestorID), "test");
    std::snprintf(row.BrokerID, sizeof(row.BrokerID), "test");
    row.HedgeFlag = r->HedgeFlag;
    row.InvestorRange = THOST_FTDC_IR_Single;
    row.IsRelative = relative_margin;
    row.LongMarginRatioByMoney = 0.1;
    row.ShortMarginRatioByMoney = 0.2;
    row.LongMarginRatioByVolume = 4;
    row.ShortMarginRatioByVolume = 8;
    spi->OnRspQryInstrumentMarginRate(&row, nullptr, id, true);
    return 0;
  }
  int ReqQryExchangeMarginRate(CThostFtdcQryExchangeMarginRateField* r,
                               int id) override {
    EXPECT_TRUE(relative_margin);
    EXPECT_STREQ(r->InstrumentID, "TF2612");
    EXPECT_STREQ(r->ExchangeID, "CFFEX");
    CThostFtdcExchangeMarginRateField row{};
    std::snprintf(row.InstrumentID, sizeof(row.InstrumentID), "TF2612");
    std::snprintf(row.BrokerID, sizeof(row.BrokerID), "test");
    row.HedgeFlag = r->HedgeFlag;
    row.LongMarginRatioByMoney = 0.025;
    row.ShortMarginRatioByVolume = 1;
    spi->OnRspQryExchangeMarginRate(&row, nullptr, id, true);
    return 0;
  }
  int ReqQryInstrumentCommissionRate(CThostFtdcQryInstrumentCommissionRateField* r,
                                     int id) override {
    EXPECT_STREQ(r->InstrumentID, "TF2612");
    EXPECT_STREQ(r->InvestorID, "test");
    EXPECT_STREQ(r->BrokerID, "test");
    CThostFtdcInstrumentCommissionRateField row{};
    std::snprintf(row.BrokerID, sizeof(row.BrokerID), "test");
    std::snprintf(row.InstrumentID, sizeof(row.InstrumentID), "%s", fee_symbol);
    row.InvestorRange = THOST_FTDC_IR_All;
    row.OpenRatioByMoney = 0.001;
    row.OpenRatioByVolume = 3;
    row.CloseRatioByVolume = 2;
    if (wrong_investor) {
      row.InvestorRange = THOST_FTDC_IR_Single;
      std::snprintf(row.InvestorID, sizeof(row.InvestorID), "other");
    }
    spi->OnRspQryInstrumentCommissionRate(missing_fees ? nullptr : &row,
                                          nullptr, id, true);
    return 0;
  }
  int ReqQryOrder(CThostFtdcQryOrderField* r, int id) override {
    EXPECT_STREQ(r->BrokerID, "test");
    EXPECT_STREQ(r->InvestorID, "test");
    EXPECT_STREQ(r->InstrumentID, "");
    ++order_queries;
    for (auto& order : history_orders) spi->OnRspQryOrder(&order, nullptr, id, false);
    spi->OnRspQryOrder(nullptr, nullptr, id, true);
    return 0;
  }
  int ReqQryTrade(CThostFtdcQryTradeField* r, int id) override {
    EXPECT_STREQ(r->BrokerID, "test");
    EXPECT_STREQ(r->InvestorID, "test");
    EXPECT_STREQ(r->InstrumentID, "");
    ++trade_queries;
    for (auto& trade : history_trades) spi->OnRspQryTrade(&trade, nullptr, id, false);
    spi->OnRspQryTrade(nullptr, nullptr, id, true);
    return 0;
  }
#include <unused_trader_calls.inc>
  int multiplier = 10;
  std::vector<std::string> position_contract_queries, detail_queries;
  std::vector<CThostFtdcInvestorPositionField> position_rows;
  std::vector<CThostFtdcInvestorPositionDetailField> details;
  int order_queries = 0, trade_queries = 0;
  std::vector<CThostFtdcOrderField> history_orders;
  std::vector<CThostFtdcTradeField> history_trades;
  bool relative_margin = false, missing_fees = false, wrong_investor = false;
  const char* fee_symbol = "TF2612";

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
  CThostFtdcTradingAccountField account_balance = Balance();
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
  for (int i = 0; i < 4; ++i) session->Run();
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
  EXPECT_FALSE(snapshot->holds_error.empty());
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
  for (int i = 0; i < 4; ++i) session->Run();
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

// Exercises the native SDK request sequence and the local accounting consumer.
class ContractTermsTest : public StartupSnapshotTest {
 protected:
  void Query() {
    probe->automatic = true;
    ASSERT_TRUE(session->StartSnapshot("TF2612"));
    for (int i = 0; i < 30; ++i) session->Run();
  }
};

TEST_F(ContractTermsTest, AppliesDirectionalTermsToAnActualReservation) {
  Query();
  ASSERT_TRUE(session->snapshot() && session->snapshot()->terms);
  const auto& terms = *session->snapshot()->terms;
  EXPECT_EQ(terms.symbol, "TF2612");
  EXPECT_EQ(terms.buy.currency.code(), "CNY");
  EXPECT_EQ(terms.buy.contract_multiplier, 10);
  EXPECT_DOUBLE_EQ(*terms.sell.margin_rate, 0.2);
  EXPECT_DOUBLE_EQ(terms.sell.margin_per_quantity, 8);
  SimulatedTradingAdapter counter;
  counter.SetInitialState({model::Currency("CNY"), 1000, 1000, 0, 0}, {});
  Outcomes listener;
  TradingService service;
  ASSERT_TRUE(service.BindAccount(7, counter));
  ASSERT_TRUE(service.ConfigureFundsTerms(7, 42, Side::kBuy, terms.buy));
  TradingServiceRegistry registry;
  ASSERT_TRUE(registry.Register(3, service));
  auto client = TradingClientFactory(registry).Create(3);
  service.Run();
  SubmitOrderRequest order{{{3, 1}, 7, 4, 42, Side::kBuy, Offset::kOpen, 100, 2}};
  ASSERT_EQ(client->SubmitOrder(order, listener), RequestResult::kDelivered);
  FundsSnapshot funds;
  ASSERT_TRUE(client->QueryFunds({7}, funds));
  // 2 * 100 * 10 * 0.1 + 2 * 4 margin, plus 2000 * 0.001 + 2 * 3 fees.
  EXPECT_DOUBLE_EQ(funds.reserved, 216);
  EXPECT_DOUBLE_EQ(funds.available_for_orders(), 784);
  const auto close_cost = [&](Offset offset) {
    order.order.offset = offset;
    return internal::CalculateOrderCost(order.order, terms.sell, 100, 2);
  };
  for (auto offset : {Offset::kClose, Offset::kCloseYesterday}) {
    ASSERT_TRUE(close_cost(offset));
    EXPECT_EQ(close_cost(offset)->total(), 4);
  }
  ASSERT_TRUE(close_cost(Offset::kCloseToday));
  EXPECT_EQ(close_cost(Offset::kCloseToday)->total(), 0);
  order.order.hedge = HedgeFlag::kHedge;
  EXPECT_FALSE(close_cost(Offset::kOpen));
}

TEST_F(ContractTermsTest, RelativeMarginIncludesTheExchangeBase) {
  probe->relative_margin = true;
  Query();
  ASSERT_TRUE(session->snapshot() && session->snapshot()->terms);
  EXPECT_DOUBLE_EQ(*session->snapshot()->terms->buy.margin_rate, 0.125);
  EXPECT_DOUBLE_EQ(session->snapshot()->terms->sell.margin_per_quantity, 9);
}

TEST_F(ContractTermsTest, MissingOrUnrelatedRateNeverPublishesPartialTerms) {
  probe->missing_fees = true;
  Query();
  EXPECT_FALSE(session->snapshot());
  EXPECT_STREQ(session->snapshot_status().reason, "commission");
  probe->missing_fees = false;
  probe->wrong_investor = true;
  Query();
  EXPECT_FALSE(session->snapshot());
  probe->wrong_investor = false;
  probe->fee_symbol = "OTHER";
  Query();
  EXPECT_FALSE(session->snapshot());
  probe->fee_symbol = "TF";
  Query();
  ASSERT_TRUE(session->snapshot() && session->snapshot()->terms);
}

// Replays cabinet query rows through the registered SPI with no local book.
class OrderHistoryTest : public StartupSnapshotTest {
 protected:
  CThostFtdcOrderField Order(int session_id, int system,
                            std::optional<SimNowOrderOwner> owner) {
    CThostFtdcOrderField row{};
    std::strcpy(row.TradingDay, "20261103");
    std::strcpy(row.ExchangeID, "SHFE");
    std::strcpy(row.BrokerID, "test");
    std::strcpy(row.InvestorID, "test");
    std::strcpy(row.OrderRef, "1");
    if (system) std::snprintf(row.OrderSysID, sizeof(row.OrderSysID), "%d", system);
    std::strcpy(row.InstrumentID, "al2612");
    row.FrontID = 2;
    row.SessionID = session_id;
    row.LimitPrice = 100;
    row.VolumeTotalOriginal = row.VolumeTotal = 3;
    row.Direction = THOST_FTDC_D_Buy;
    row.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
    row.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
    row.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    row.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    row.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    if (owner) {
      CThostFtdcInputOrderField sent{};
      EXPECT_TRUE(internal::WriteOrderTag(*owner, sent));
      std::strcpy(row.BusinessUnit, sent.BusinessUnit);
      std::strcpy(row.OrderMemo, sent.OrderMemo);
    }
    return row;
  }
  CThostFtdcTradeField Fill(const CThostFtdcOrderField& source) {
    CThostFtdcTradeField row{};
    std::strcpy(row.TradingDay, source.TradingDay);
    std::strcpy(row.TradeID, "fill-1");
    std::strcpy(row.ExchangeID, source.ExchangeID);
    std::strcpy(row.BrokerID, source.BrokerID);
    std::strcpy(row.InvestorID, source.InvestorID);
    std::strcpy(row.OrderSysID, source.OrderSysID);
    std::strcpy(row.InstrumentID, source.InstrumentID);
    row.OffsetFlag = source.CombOffsetFlag[0];
    row.HedgeFlag = source.CombHedgeFlag[0];
    row.Direction = source.Direction;
    row.Price = 99;
    row.Volume = 1;
    return row;
  }
  void Query() {
    probe->automatic = true;
    ASSERT_TRUE(session->StartSnapshot());
    for (int i = 0; i < 60; ++i) session->Run();
  }
};

TEST_F(OrderHistoryTest, RestoresOwnersAndAssociatesTradesWithoutRefCollisions) {
  SimNowOrderOwner first{{5, 11}, 31}, second{{5, 18}, 32};
  auto a = Order(10, 21, first), b = Order(11, 22, second);
  a.VolumeTraded = 1;
  a.VolumeTotal = 2;
  a.OrderStatus = THOST_FTDC_OST_PartTradedQueueing;
  auto unowned = Order(12, 0, std::nullopt);
  probe->history_orders = {a, b, unowned, a};
  auto fill = Fill(a);
  std::strcpy(fill.OrderSysID, "    21");
  probe->history_trades = {fill, fill};
  Query();
  ASSERT_TRUE(session->snapshot());
  auto history = session->snapshot()->history;
  ASSERT_EQ(history.orders().size(), 3);
  ASSERT_EQ(history.fills().size(), 1);
  EXPECT_EQ(history.orders()[history.fills().begin()->second.order_index].owner, first);
  ASSERT_NE(history.Find(2, 12, "1"), nullptr);
  EXPECT_FALSE(history.Find(2, 12, "1")->owner);
  EXPECT_EQ(history.Find("SHFE", "22"), history.Find(second.order_id));
  EXPECT_EQ(history.Find(2, 10, "1"), history.Find(first.order_id));

  OrderIdGenerator generator(5, history.LastSequence(5));
  auto next = generator.Next();
  ASSERT_TRUE(next);
  EXPECT_EQ(next->sequence, 19);
  auto fresh = history.orders().front();
  fresh.session = 13;
  fresh.system_id = "23";
  fresh.owner = SimNowOrderOwner{*next, 31};
  ASSERT_TRUE(history.AddOrder(fresh));
  EXPECT_EQ(history.Find(2, 13, "1"), history.Find(*next));
  EXPECT_EQ(history.Find("SHFE", "21"), history.Find(first.order_id));
  TradeId subsequent{"SHFE", "20261103/0", "fill-2"};
  ASSERT_TRUE(history.AddFill(subsequent, "23", fresh.symbol, fresh.direction,
                              fresh.offset, fresh.hedge, 101, 1));
  const auto& later = history.fills().at(subsequent);
  EXPECT_EQ(history.orders()[later.order_index].owner->order_id, *next);
  EXPECT_EQ(probe->order_queries, 1);
  EXPECT_EQ(probe->trade_queries, 1);
}

TEST_F(OrderHistoryTest, TagPreservesFullWidthAndRejectsDamagedOrForeignTags) {
  auto max = std::numeric_limits<std::uint64_t>::max();
  SimNowOrderOwner owner{{max, max - 1}, max - 2};
  auto order = Order(1, 1, owner);
  EXPECT_EQ(internal::ReadOrderTag(order), owner);
  order.OrderMemo[11] = 0;
  EXPECT_FALSE(internal::ReadOrderTag(order));
  order = Order(1, 1, owner);
  order.BusinessUnit[0] = 'X';
  EXPECT_FALSE(internal::ReadOrderTag(order));
  order = Order(1, 1, owner);
  order.OrderMemo[5] = ' ';
  EXPECT_FALSE(internal::ReadOrderTag(order));
}

TEST_F(OrderHistoryTest, FailsAtomicallyOnConflictingIdsOrUnmatchedExecutions) {
  auto a = Order(1, 1, SimNowOrderOwner{{3, 4}, 5});
  auto b = Order(2, 2, SimNowOrderOwner{{3, 4}, 6});
  probe->history_orders = {a, b};
  Query();
  EXPECT_FALSE(session->snapshot());
  EXPECT_STREQ(session->snapshot_status().reason, "invalid_order_history");
  probe->history_orders = {a};
  probe->history_trades = {Fill(b)};
  Query();
  EXPECT_FALSE(session->snapshot());
  auto t = Fill(a);
  auto conflicting = t;
  conflicting.Price++;
  probe->history_trades = {t, conflicting};
  Query();
  EXPECT_FALSE(session->snapshot());
  EXPECT_STREQ(session->snapshot_status().reason, "invalid_trade_history");
  probe->history_trades = {t};
  Query();
  ASSERT_TRUE(session->snapshot());
  EXPECT_EQ(session->snapshot()->history.fills().size(), 1);
}

// Exercises position verification through the session's native SDK callbacks.
class PositionCostsTest : public StartupSnapshotTest {
 protected:
  void Query() {
    probe->automatic = true;
    ASSERT_TRUE(session->StartSnapshot());
    for (int i = 0; i < 100; ++i) session->Run();
  }
  CThostFtdcInvestorPositionDetailField Detail(
      const CThostFtdcInvestorPositionField& source, const char* date,
      const char* trade, int volume, double open, double settlement, double margin) {
    CThostFtdcInvestorPositionDetailField row{};
    std::strcpy(row.BrokerID, source.BrokerID);
    std::strcpy(row.InvestorID, source.InvestorID);
    std::strcpy(row.InstrumentID, source.InstrumentID);
    std::strcpy(row.ExchangeID, source.ExchangeID);
    std::strcpy(row.TradingDay, source.TradingDay);
    std::strcpy(row.InvestUnitID, source.InvestUnitID);
    std::strcpy(row.OpenDate, date);
    std::strcpy(row.TradeID, trade);
    row.HedgeFlag = source.HedgeFlag;
    row.Direction = source.PosiDirection == THOST_FTDC_PD_Long
                        ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
    row.Volume = volume;
    row.OpenPrice = open;
    row.LastSettlementPrice = settlement;
    row.Margin = margin;
    return row;
  }
  void Mixed(int symbol, bool short_side = false) {
    auto r = Position(symbol);
    std::strcpy(r.ExchangeID, "DCE");
    r.Position = 5;
    r.TodayPosition = 2;
    r.YdPosition = 6;  // Some prior-day lots have already been closed.
    r.PositionCost = 5150;
    r.UseMargin = 41;
    if (short_side) r.PosiDirection = THOST_FTDC_PD_Short;
    probe->position_rows.push_back(r);
    probe->details.push_back(Detail(r, "20261103", "a", 1, 100, 90, 10));
    probe->details.push_back(Detail(r, "20261103", "b", 1, 115, 90, 11));
    probe->details.push_back(Detail(r, "20261029", "a", 3, 80, 100, 20));
  }
};

TEST_F(PositionCostsTest, SingleDateRowsUseReportedAmountsWithoutDetailQueries) {
  auto today = Position(1);
  auto prior = today;
  prior.PositionDate = THOST_FTDC_PSD_History;
  prior.TodayPosition = 0;
  prior.YdPosition = 6;
  prior.PositionCost = 650;
  prior.UseMargin = 21;
  probe->position_rows = {today, prior};
  Query();
  ASSERT_TRUE(session->snapshot());
  ASSERT_EQ(session->snapshot()->positions.size(), 2);
  for (const auto& r : session->snapshot()->positions) {
    if (r.today) {
      EXPECT_EQ(r.today_cost, (PositionCost{400, 40}));
      EXPECT_EQ(r.yesterday_cost, PositionCost{});
    } else {
      EXPECT_EQ(r.yesterday_quantity(), 4);
      EXPECT_EQ(r.yesterday_cost, (PositionCost{650, 21}));
      EXPECT_EQ(r.today_cost, PositionCost{});
    }
  }
  EXPECT_TRUE(probe->detail_queries.empty());
  EXPECT_TRUE(probe->position_contract_queries.empty());
}

TEST_F(PositionCostsTest, MixedRowsRecoverCostsPerContractAndSide) {
  Mixed(1);
  Mixed(1, true);
  Mixed(2);
  Query();
  ASSERT_TRUE(session->snapshot());
  EXPECT_EQ(probe->detail_queries.size(), 2);
  EXPECT_EQ(probe->position_contract_queries, probe->detail_queries);
  ASSERT_EQ(session->snapshot()->positions.size(), 3);
  for (const auto& r : session->snapshot()->positions) {
    EXPECT_EQ(r.today, 2);
    EXPECT_EQ(r.yesterday_quantity(), 3);
    EXPECT_EQ(r.today_cost, (PositionCost{2150, 21}));
    EXPECT_EQ(r.yesterday_cost, (PositionCost{3000, 20}));
  }
}

TEST_F(PositionCostsTest, FailedDetailAttemptIsDiscardedBeforeRetry) {
  Mixed(1);
  auto& lot = probe->details.back();
  lot.Volume -= 1;
  Query();
  EXPECT_FALSE(session->snapshot());
  EXPECT_STREQ(session->snapshot_status().reason, "position_detail_quantity_mismatch");
  lot.Volume += 1;
  lot.LastSettlementPrice += 1;
  Query();
  EXPECT_FALSE(session->snapshot());
  EXPECT_STREQ(session->snapshot_status().reason, "position_detail_cost_mismatch");
  lot.LastSettlementPrice -= 1;
  lot.Margin += 1;
  Query();
  EXPECT_FALSE(session->snapshot());
  EXPECT_STREQ(session->snapshot_status().reason, "position_detail_margin_mismatch");
  lot.Margin -= 1;
  Query();
  ASSERT_TRUE(session->snapshot());
  EXPECT_EQ(session->snapshot()->positions[0].yesterday_cost.margin, 20);
}

TEST_F(PositionCostsTest, RejectsInvalidMultiplierDuplicateAndForeignDetails) {
  Mixed(1);
  probe->multiplier = 0;
  Query();
  EXPECT_FALSE(session->snapshot());
  EXPECT_STREQ(session->snapshot_status().reason, "invalid_position_multiplier");
  probe->multiplier = 10;
  probe->details.push_back(probe->details.back());
  Query();
  EXPECT_FALSE(session->snapshot());
  EXPECT_STREQ(session->snapshot_status().reason, "duplicate_position_detail");
  probe->details.pop_back();
  std::strcpy(probe->details[0].InvestorID, "foreign");
  Query();
  EXPECT_FALSE(session->snapshot());
  EXPECT_STREQ(session->snapshot_status().reason, "position_details_response");
}

class OrderHoldsTest : public OrderHistoryTest {
 protected:
  void SetUp() override {
    OrderHistoryTest::SetUp();
    auto open = Order(1, 1, SimNowOrderOwner{{5, 11}, 31});
    std::strcpy(open.InstrumentID, "TF2612");
    std::strcpy(open.ExchangeID, "CFFEX");
    open.VolumeTraded = 1;
    open.VolumeTotal = 2;
    open.OrderStatus = THOST_FTDC_OST_PartTradedQueueing;
    auto close = Order(2, 2, std::nullopt);
    std::strcpy(close.InstrumentID, "TF2612");
    std::strcpy(close.ExchangeID, "CFFEX");
    close.Direction = THOST_FTDC_D_Sell;
    close.CombOffsetFlag[0] = THOST_FTDC_OF_Close;
    auto canceled = Order(3, 3, std::nullopt);
    canceled.OrderStatus = THOST_FTDC_OST_Canceled;
    probe->history_orders = {open, close, canceled};
    probe->history_trades = {Fill(open)};
    auto today = Position(1);
    std::strcpy(today.InstrumentID, "TF2612");
    std::strcpy(today.ExchangeID, "CFFEX");
    today.ShortFrozen = 1;
    auto prior = today;
    prior.PositionDate = THOST_FTDC_PSD_History;
    prior.TodayPosition = 0;
    prior.ShortFrozen = 2;
    probe->position_rows = {today, prior};
    // Two remaining opening lots, each with 104 margin and 4 commission.
    // Three remaining closing lots carry 2 commission per lot.
    probe->account_balance.FrozenMargin = 208;
    probe->account_balance.FrozenCommission = 14;
  }
};

TEST_F(OrderHoldsTest, PublishesOnlyRemainingHoldsAndPreservesCounterTotals) {
  Query();
  const auto* s = session->snapshot();
  ASSERT_TRUE(s);
  ASSERT_TRUE(s->holds_error.empty()) << s->holds_error;
  ASSERT_EQ(s->holds.size(), 2);
  const auto& open = s->holds[0];
  EXPECT_EQ(open.order_index, 0);
  EXPECT_EQ(open.remaining, 2);
  EXPECT_EQ(open.margin, 208);
  EXPECT_EQ(open.commission, 8);
  EXPECT_EQ(open.today + open.yesterday, 0);
  const auto& close = s->holds[1];
  EXPECT_FALSE(s->history.orders()[close.order_index].owner);
  EXPECT_EQ(close.margin, 0);
  EXPECT_EQ(close.commission, 6);
  EXPECT_EQ(close.today, 1);
  EXPECT_EQ(close.yesterday, 2);
  EXPECT_EQ(s->balances[0].frozen_margin, 208);
  EXPECT_EQ(s->balances[0].frozen_commission, 14);
  EXPECT_EQ(s->balances[0].available, 59);
  EXPECT_EQ(probe->position_contract_queries.size(), 1);
  EXPECT_FALSE(session->StartSnapshot());
}

TEST_F(OrderHoldsTest, RejectsOffsettingCashErrorsAndCanRetry) {
  --probe->account_balance.FrozenMargin;
  ++probe->account_balance.FrozenCommission;
  Query();
  ASSERT_TRUE(session->snapshot());
  EXPECT_EQ(session->snapshot()->holds_error, "frozen_margin_mismatch CNY");
  EXPECT_TRUE(session->snapshot()->holds.empty());
  ++probe->account_balance.FrozenMargin;
  Query();
  EXPECT_EQ(session->snapshot()->holds_error, "frozen_commission_mismatch CNY");
  --probe->account_balance.FrozenCommission;
  Query();
  EXPECT_EQ(session->snapshot()->holds.size(), 2);
}

TEST_F(OrderHoldsTest, AmbiguousPerOrderSplitNeverPublishesPartialResults) {
  auto& close = probe->history_orders[1];
  auto another = close;
  another.SessionID = 4;
  std::strcpy(another.OrderSysID, "4");
  close.VolumeTotalOriginal = close.VolumeTotal = 2;
  another.VolumeTotalOriginal = another.VolumeTotal = 1;
  probe->history_orders.push_back(another);
  Query();
  ASSERT_TRUE(session->snapshot());
  EXPECT_EQ(session->snapshot()->holds_error, "ambiguous_close_allocation TF2612");
  EXPECT_TRUE(session->snapshot()->holds.empty());
  probe->history_orders.back().CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday;
  probe->account_balance.FrozenCommission -= 2;  // Today's closing rate is zero.
  Query();
  ASSERT_EQ(session->snapshot()->holds.size(), 3);
  EXPECT_EQ(session->snapshot()->holds[1].yesterday, 2);
  EXPECT_EQ(session->snapshot()->holds[2].today, 1);
}

TEST_F(OrderHoldsTest, BuyCloseUsesOnlyTheShortHoldingInItsInvestmentUnit) {
  auto& close = probe->history_orders[1];
  close.Direction = THOST_FTDC_D_Buy;
  std::strcpy(close.InvestUnitID, "desk2");
  for (auto& row : probe->position_rows) {
    row.PosiDirection = THOST_FTDC_PD_Short;
    row.LongFrozen = row.ShortFrozen;
    row.ShortFrozen = 0;
  }
  Query();
  ASSERT_TRUE(session->snapshot());
  EXPECT_EQ(session->snapshot()->holds_error, "position_freeze_mismatch TF2612");
  for (auto& row : probe->position_rows) std::strcpy(row.InvestUnitID, "desk2");
  Query();
  ASSERT_EQ(session->snapshot()->holds.size(), 2);
  EXPECT_EQ(session->snapshot()->holds[1].today, 1);
  EXPECT_EQ(session->snapshot()->holds[1].yesterday, 2);
}

TEST_F(OrderHoldsTest, UnknownOrderStatePreventsVerificationEvenWhenTotalsAgree) {
  probe->history_orders.back().OrderStatus = THOST_FTDC_OST_Unknown;
  Query();
  ASSERT_TRUE(session->snapshot());
  EXPECT_EQ(session->snapshot()->holds_error, "unresolved_order_status al2612");
  EXPECT_TRUE(session->snapshot()->holds.empty());
}

TEST_F(OrderHoldsTest, UnmatchedPositionFreezeAndMissingRatesAreDiagnosed) {
  ++probe->position_rows.front().ShortFrozen;
  Query();
  ASSERT_TRUE(session->snapshot());
  EXPECT_EQ(session->snapshot()->holds_error, "position_freeze_mismatch TF2612");
  --probe->position_rows.front().ShortFrozen;
  probe->missing_fees = true;
  Query();
  ASSERT_TRUE(session->snapshot());
  EXPECT_NE(session->snapshot()->holds_error.find("missing_order_rates TF2612 commission"),
            std::string::npos);
  EXPECT_TRUE(session->snapshot()->holds.empty());
}

TEST(StartupSnapshot, MonodPublishesOnlyTheCompletedSummary) {
  const char* args[] = {"tcp://fixture:1", "2"};
  auto connect = [](const SimNowTraderConfig& config) {
    return internal::StartTraderSession(config, [] {
      auto* probe = new Probe;
      probe->automatic = true;
      probe->account_balance.FrozenCommission = probe->account_balance.FrozenMargin = 0;
      return probe;
    });
  };
  ::testing::internal::CaptureStdout();
  const int result = monod::RunTraderLogin(2, args, true, connect);
  const auto out = ::testing::internal::GetCapturedStdout();
  EXPECT_EQ(result, 0);
  EXPECT_EQ(out, "startup_holds_verified orders=0\nstartup_snapshot trading_day=20261103 currencies=1 "
                 "position_rows=0 orders=0 fills=0 account_ready=false\n");
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
