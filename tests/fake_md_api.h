#ifndef YQUANT_TESTS_FAKE_MD_API_H_
#define YQUANT_TESTS_FAKE_MD_API_H_

#include <ThostFtdcMdApi.h>

#include <cstdio>
#include <string>
#include <thread>

namespace yquant::test {

// Controlled subclass of the platform's vendor API. The test owns this object
// until the session calls Release. It never opens a socket.
class FakeMdApi final : public CThostFtdcMdApi {
 public:
  // CThostFtdcMdApi implementation.
  void RegisterSpi(CThostFtdcMdSpi* value) override { spi = value; }
  void RegisterFront(char* value) override { front = value; }
  void Init() override {
    if (automatic) spi->OnFrontConnected();
  }
  void Release() override { released_without_spi = spi == nullptr; }
  int ReqUserLogin(CThostFtdcReqUserLoginField* request, int id) override {
    ++login_calls;
    request_id = id;
    login = *request;
    login_thread = std::this_thread::get_id();
    if (automatic) LoginReply();
    return login_rc;
  }
  int SubscribeMarketData(char* symbols[], int count) override {
    ++sub_calls;
    symbol = count == 1 ? symbols[0] : "";
    sub_thread = std::this_thread::get_id();
    if (automatic) {
      std::thread producer([this] {
        SubReply(symbol.c_str());
        Tick("ignored");
        Tick(symbol.c_str());
        Tick(symbol.c_str(), 102, 103);
      });
      producer.join();
    }
    return sub_rc;
  }
  int Join() override { return 0; }
  const char* GetTradingDay() override { return ""; }
  void RegisterNameServer(char*) override {}
  void RegisterFensUserInfo(CThostFtdcFensUserInfoField*) override {}
  int UnSubscribeMarketData(char*[], int) override { return -1; }
  int SubscribeForQuoteRsp(char*[], int) override { return -1; }
  int UnSubscribeForQuoteRsp(char*[], int) override { return -1; }
  int ReqUserLogout(CThostFtdcUserLogoutField*, int) override { return -1; }
  int ReqQryMulticastInstrument(CThostFtdcQryMulticastInstrumentField*,
                                int) override { return -1; }

  void LoginReply(int error = 0) {
    CThostFtdcRspInfoField info{};
    info.ErrorID = error;
    CThostFtdcRspUserLoginField reply{};
    spi->OnRspUserLogin(&reply, &info, request_id, true);
  }
  void SubReply(const char* name, int error = 0) {
    CThostFtdcSpecificInstrumentField reply{};
    std::snprintf(reply.InstrumentID, sizeof(reply.InstrumentID), "%s", name);
    CThostFtdcRspInfoField info{};
    info.ErrorID = error;
    spi->OnRspSubMarketData(&reply, &info, 0, true);
  }
  void Tick(const char* name, double bid = 100, double ask = 101, int qty = 3) {
    CThostFtdcDepthMarketDataField tick{};
    std::snprintf(tick.InstrumentID, sizeof(tick.InstrumentID), "%s", name);
    tick.BidPrice1 = bid;
    tick.AskPrice1 = ask;
    tick.BidVolume1 = qty;
    tick.AskVolume1 = 5;
    spi->OnRtnDepthMarketData(&tick);
  }

  CThostFtdcMdSpi* spi = nullptr;
  CThostFtdcReqUserLoginField login{};
  std::string front, symbol;
  int request_id = 0, login_calls = 0, sub_calls = 0, login_rc = 0, sub_rc = 0;
  bool automatic = false, released_without_spi = false;
  std::thread::id login_thread, sub_thread;
};

}  // namespace yquant::test

#endif  // YQUANT_TESTS_FAKE_MD_API_H_
