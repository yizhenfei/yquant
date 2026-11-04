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
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
      : front_(c.front) {}
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
        front_.find('\0') != front_.npos ||
        (!c.subscriptions.empty() && (!c.symbol.empty() || c.instrument_id != 0)) ||
        c.subscriptions.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return false;
    }
    instruments_.reserve(c.subscriptions.empty() ? 1 : c.subscriptions.size());
    if (c.subscriptions.empty()) {
      instruments_.push_back({std::string(c.symbol), c.instrument_id});
    } else {
      for (const auto& s : c.subscriptions) instruments_.push_back({s.symbol, s.instrument_id});
    }
    std::unordered_set<std::uint32_t> ids;
    index_.reserve(instruments_.size());
    subscriptions_.reserve(instruments_.size());
    for (std::size_t i = 0; i < instruments_.size(); ++i) {
      auto& s = instruments_[i];
      if (!s.id || s.symbol.empty() || s.symbol.size() >= width ||
          s.symbol.find_first_not_of(allowed) != s.symbol.npos ||
          !ids.insert(s.id).second || !index_.emplace(s.symbol, i).second) return false;
      subscriptions_.push_back(s.symbol.data());
    }
    if (!c.anonymous && (!CopyField(login_.BrokerID, c.broker) ||
                         !CopyField(login_.UserID, c.user) ||
                         !CopyField(login_.Password, c.password))) return false;
    api_ = create();
    if (!api_) return false;
    api_->RegisterSpi(this);
    api_->RegisterFront(front_.data());
    api_->Init();
    return true;
  }

  // QuoteSource implementation.
  bool TryRead(model::Quote& out) override {
    const auto phase = phase_.load();
    if (phase == Phase::kConnected && Move(phase, Phase::kLogin)) {
      std::puts("simnow_connected");
      std::fflush(stdout);
      const int rc = api_->ReqUserLogin(&login_, 1);
      if (rc != 0) Fail("login_request", rc);
    } else if (phase == Phase::kLoggedIn && Move(phase, Phase::kSubscribe)) {
      std::puts("simnow_logged_in");
      std::fflush(stdout);
      const int rc = api_->SubscribeMarketData(subscriptions_.data(), static_cast<int>(subscriptions_.size()));
      if (rc != 0) Fail("subscribe_request", rc);
    }
    if (phase_ != Phase::kStreaming) return false;
    std::unique_lock lock(queue_mutex_, std::try_to_lock);
    if (!lock || count_ == 0 || phase_ != Phase::kStreaming) {
      return false;
    }
    out = queue_[head_];
    head_ = (head_ + 1) % queue_.size();
    --count_;
    return true;
  }

  // SimNowQuoteSource implementation.
  bool failed() const override { return phase_ == Phase::kFailed; }
  bool ready() const override { return phase_ == Phase::kStreaming; }

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
    std::lock_guard lock(queue_mutex_);
    if (phase_ != Phase::kSubscribe && phase_ != Phase::kStreaming) return;
    if (error && error->ErrorID) {
      Fail("subscribe", error->ErrorID, contract ? Field(contract->InstrumentID) : "<missing>");
      return;
    }
    if (contract) {
      const auto it = index_.find(Field(contract->InstrumentID));
      if (it == index_.end()) { Fail("subscription_mismatch", -1); return; }
      auto& instrument = instruments_[it->second];
      if (!instrument.confirmed) {
        instrument.confirmed = true;
        ++confirmed_;
      }
    } else if (!last || confirmed_ != instruments_.size()) {
      Fail("missing_subscription_response", -1);
      return;
    }
    final_seen_ |= last;
    if (final_seen_ && confirmed_ == instruments_.size() &&
        Move(Phase::kSubscribe, Phase::kStreaming)) {
      if (instruments_.size() == 1) {
        std::printf("simnow_subscribed symbol=%s\n", instruments_[0].symbol.c_str());
      } else {
        std::printf("simnow_subscribed contracts=%zu\n", instruments_.size());
      }
      std::fflush(stdout);
    }
  }
  void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* tick) override {
    if (phase_ != Phase::kStreaming || !tick) return;
    const auto found = index_.find(Field(tick->InstrumentID));
    if (found == index_.end()) return;
    const double bid = tick->BidPrice1, ask = tick->AskPrice1;
    if (!std::isfinite(bid) || !std::isfinite(ask) || bid <= 0 || bid >= ask ||
        ask == std::numeric_limits<double>::max() || tick->BidVolume1 <= 0 ||
        tick->AskVolume1 <= 0) return;
    std::lock_guard lock(queue_mutex_);
    if (phase_ != Phase::kStreaming) return;
    auto& instrument = instruments_[found->second];
    if (count_ == queue_.size() || instrument.sequence == UINT64_MAX) {
      Fail("quote_capacity_exhausted", -1);
      return;
    }
    queue_[(head_ + count_) % queue_.size()] =
        {instrument.id, bid, ask, tick->BidVolume1, tick->AskVolume1, ++instrument.sequence};
    ++count_;
  }

  bool Move(Phase from, Phase to) {
    return phase_.compare_exchange_strong(from, to);
  }
  void Fail(const char* reason, int code, std::string_view symbol = {}) {
    auto phase = phase_.load();
    while (phase != Phase::kFailed && phase != Phase::kStopping) {
      if (phase_.compare_exchange_weak(phase, Phase::kFailed)) {
        std::fprintf(stderr, "simnow_market %s code=%d", reason, code);
        if (!symbol.empty()) std::fprintf(stderr, " symbol=%.*s", static_cast<int>(symbol.size()), symbol.data());
        std::fputc('\n', stderr);
        return;
      }
    }
  }

  // Strings and the index are frozen before Init; views and SDK pointers
  // remain valid until Release finishes. Callback counters use queue_mutex_.
  struct Instrument {
    std::string symbol;
    std::uint32_t id;
    bool confirmed = false;
    std::uint64_t sequence = 0;
  };
  std::string front_;
  std::vector<Instrument> instruments_;
  std::unordered_map<std::string_view, std::size_t> index_;
  std::vector<char*> subscriptions_;
  std::size_t confirmed_ = 0;
  bool final_seen_ = false;
  CThostFtdcReqUserLoginField login_{};
  CThostFtdcMdApi* api_ = nullptr;
  std::atomic<Phase> phase_{Phase::kConnecting};
  std::mutex queue_mutex_;
  std::array<model::Quote, 1024> queue_{};
  std::size_t head_ = 0, count_ = 0;
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
