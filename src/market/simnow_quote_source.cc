#include "yquant/market/simnow_quote_source.h"

#include <ThostFtdcMdApi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>

#include "simnow_session.h"

namespace yquant::market {
namespace {

enum class Phase {
  kConnecting, kConnected, kLogin, kLoggedIn, kSubscribe, kStreaming,
  kFailed, kStopping
};

// CTP fields can fill their whole array without a terminating NUL.
template <std::size_t N>
std::string_view Field(const char (&field)[N]) {
  return {field, static_cast<std::size_t>(std::find(field, field + N, '\0') -
                                         field)};
}

template <std::size_t N>
bool CopyField(char (&out)[N], std::string_view value) {
  if (value.empty() || value.size() >= N ||
      value.find('\0') != value.npos) return false;
  std::copy(value.begin(), value.end(), out);
  out[value.size()] = '\0';
  return true;
}

// Owns the API, SPI and callback storage for one connection. Only the business
// thread calls API methods. Release stops SDK workers before storage expires.
class Session final : public SimNowQuoteSource, private CThostFtdcMdSpi {
 public:
  explicit Session(const SimNowMarketConfig& c)
      : front_(c.front), symbol_(c.symbol), instrument_(c.instrument_id) {}
  ~Session() override {
    phase_ = Phase::kStopping;
    if (api_) {
      api_->RegisterSpi(nullptr);
      api_->Release();
    }
  }

  // Starts callbacks only after the complete login request has been validated.
  bool Start(const SimNowMarketConfig& c,
             const std::function<CThostFtdcMdApi*()>& create) {
    constexpr auto allowed =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.";
    constexpr auto width = sizeof(CThostFtdcSpecificInstrumentField::InstrumentID);
    if (!front_.starts_with("tcp://") || front_.size() <= 6 ||
        front_.find('\0') != front_.npos || instrument_ == 0 ||
        symbol_.empty() || symbol_.size() >= width ||
        symbol_.find_first_not_of(allowed) != symbol_.npos) return false;
    if (!c.anonymous && (!CopyField(login_.BrokerID, c.broker) ||
                         !CopyField(login_.UserID, c.user) ||
                         !CopyField(login_.Password, c.password))) return false;
    api_ = create();
    if (!api_) return false;
    subscription_ = symbol_.data();
    api_->RegisterSpi(this);
    api_->RegisterFront(front_.data());
    api_->Init();
    return true;
  }

  // QuoteSource implementation.
  QuoteReadResult Read(model::Quote& out) override {
    const auto phase = phase_.load();
    if (phase == Phase::kConnected && Move(phase, Phase::kLogin)) {
      std::puts("simnow_connected");
      std::fflush(stdout);
      const int rc = api_->ReqUserLogin(&login_, 1);
      if (rc != 0) Fail("login_request", rc);
    } else if (phase == Phase::kLoggedIn && Move(phase, Phase::kSubscribe)) {
      std::puts("simnow_logged_in");
      std::fflush(stdout);
      const int rc = api_->SubscribeMarketData(&subscription_, 1);
      if (rc != 0) Fail("subscribe_request", rc);
    }
    if (phase_ != Phase::kStreaming) return QuoteReadResult::kIdle;
    std::unique_lock lock(queue_mutex_, std::try_to_lock);
    if (!lock || count_ == 0 || phase_ != Phase::kStreaming) {
      return QuoteReadResult::kIdle;
    }
    out = queue_[head_];
    head_ = (head_ + 1) % queue_.size();
    --count_;
    return QuoteReadResult::kQuote;
  }

  // SimNowQuoteSource implementation.
  bool failed() const override { return phase_ == Phase::kFailed; }

 private:
  // CThostFtdcMdSpi implementation.
  void OnFrontConnected() override {
    Move(Phase::kConnecting, Phase::kConnected);
  }
  void OnFrontDisconnected(int reason) override { Fail("disconnect", reason); }
  void OnRspError(CThostFtdcRspInfoField* error, int, bool) override {
    Fail("response_error", error ? error->ErrorID : -1);
  }
  void OnRspUserLogin(CThostFtdcRspUserLoginField* reply,
                      CThostFtdcRspInfoField* error, int id,
                      bool last) override {
    if (phase_ != Phase::kLogin) return;
    // SimNow MD can return zero instead of echoing the sole login request ID.
    if (id != 0 && id != 1) {
      std::fprintf(stderr, "simnow_market unexpected_login_response id=%d\n", id);
      return;
    }
    if (error && error->ErrorID) {
      Fail("login", error->ErrorID);
    } else if (last && !reply) {
      Fail("missing_login_response", -1);
    } else if (last) {
      Move(Phase::kLogin, Phase::kLoggedIn);
    }
  }
  void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* contract,
                          CThostFtdcRspInfoField* error, int,
                          bool last) override {
    if (phase_ != Phase::kSubscribe) return;
    if (error && error->ErrorID) {
      Fail("subscribe", error->ErrorID);
    } else if (last && (!contract || Field(contract->InstrumentID) != symbol_)) {
      Fail("subscription_mismatch", -1);
    } else if (last && Move(Phase::kSubscribe, Phase::kStreaming)) {
      std::printf("simnow_subscribed symbol=%s\n", symbol_.c_str());
      std::fflush(stdout);
    }
  }
  void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* tick) override {
    if (phase_ != Phase::kStreaming || !tick ||
        Field(tick->InstrumentID) != symbol_) return;
    const double bid = tick->BidPrice1, ask = tick->AskPrice1;
    if (!std::isfinite(bid) || !std::isfinite(ask) || bid <= 0 || bid >= ask ||
        ask == std::numeric_limits<double>::max() || tick->BidVolume1 <= 0 ||
        tick->AskVolume1 <= 0) return;
    std::lock_guard lock(queue_mutex_);
    if (phase_ != Phase::kStreaming) return;
    if (count_ == queue_.size() || seq_ == UINT64_MAX) {
      Fail("quote_capacity_exhausted", -1);
      return;
    }
    queue_[(head_ + count_) % queue_.size()] =
        {instrument_, bid, ask, tick->BidVolume1, tick->AskVolume1, ++seq_};
    ++count_;
  }

  bool Move(Phase from, Phase to) {
    return phase_.compare_exchange_strong(from, to);
  }
  void Fail(const char* reason, int code) {
    auto phase = phase_.load();
    while (phase != Phase::kFailed && phase != Phase::kStopping) {
      if (phase_.compare_exchange_weak(phase, Phase::kFailed)) {
        std::fprintf(stderr, "simnow_market %s code=%d\n", reason, code);
        return;
      }
    }
  }

  std::string front_, symbol_;
  std::uint32_t instrument_;
  CThostFtdcReqUserLoginField login_{};
  CThostFtdcMdApi* api_ = nullptr;
  char* subscription_ = nullptr;
  std::atomic<Phase> phase_{Phase::kConnecting};
  std::mutex queue_mutex_;
  std::array<model::Quote, 1024> queue_{};
  std::size_t head_ = 0, count_ = 0;
  std::uint64_t seq_ = 0;
};

}  // namespace

std::unique_ptr<SimNowQuoteSource> internal::StartSession(
    const SimNowMarketConfig& config,
    const std::function<CThostFtdcMdApi*()>& create) {
  auto session = std::make_unique<Session>(config);
  if (!session->Start(config, create)) return nullptr;
  return session;
}

std::unique_ptr<SimNowQuoteSource> SimNowQuoteSource::Connect(
    const SimNowMarketConfig& config) {
  return internal::StartSession(config, [] {
    return CThostFtdcMdApi::CreateFtdcMdApi();
  });
}

}  // namespace yquant::market
