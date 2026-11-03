#include "yquant/trading/simnow_trading_adapter.h"

#include <ThostFtdcTraderApi.h>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <mutex>

#include "simnow_trader_session.h"

namespace yquant::trading {
namespace {

using Clock = std::chrono::steady_clock;

template <std::size_t N>
bool Copy(char (&out)[N], std::string_view in) {
  if (in.empty() || in.size() >= N || in.find('\0') != in.npos) return false;
  std::copy(in.begin(), in.end(), out);
  return true;  // Request structures are zero-initialized.
}

template <std::size_t N>
std::string_view Field(const char (&value)[N]) {
  return {value, static_cast<std::size_t>(
                     std::find(value, value + N, '\0') - value)};
}

enum class Kind { kConnect, kDisconnect, kAuth, kLogin, kError };

// Copies just the callback data needed by Run, never SDK pointers or passwords.
struct Event {
  Kind kind;
  int id = 0, code = 0;
  bool valid = false, last = false;
  int front = 0, session = 0;
  TThostFtdcDateType day{};
  TThostFtdcOrderRefType ref{};
  std::uint64_t generation = 0;
};

// Serializes SDK control events onto the business thread. Each Run handles
// one event so observers can see a disconnect before a reconnect is processed.
class Session final : public SimNowTradingAdapter, private CThostFtdcTraderSpi {
 public:
  explicit Session(const SimNowTraderConfig& c)
      : front_(c.front), broker_(c.broker), user_(c.user), timeout_(c.timeout) {}
  ~Session() override {
    if (api_) {
      api_->RegisterSpi(nullptr);
      api_->Release();
    }
  }

  bool Start(const SimNowTraderConfig& c,
             const std::function<CThostFtdcTraderApi*()>& create) {
    if (!front_.starts_with("tcp://") || front_.size() <= 6 ||
        front_.find('\0') != front_.npos || timeout_.count() <= 0 ||
        timeout_ > std::chrono::hours(1) ||
        c.system_info.size() > sizeof(TThostFtdcClientSystemInfoType) ||
        !Copy(auth_.BrokerID, c.broker) || !Copy(auth_.UserID, c.user) ||
        !Copy(auth_.AppID, c.app_id) || !Copy(auth_.AuthCode, c.auth_code) ||
        !Copy(login_.BrokerID, c.broker) || !Copy(login_.UserID, c.user) ||
        !Copy(login_.Password, c.password)) return false;
#ifdef YQUANT_CTP_MAC_LOGIN
    std::copy(c.system_info.begin(), c.system_info.end(), info_);
    info_length_ = static_cast<int>(c.system_info.size());
#else
    if (!c.system_info.empty()) return false;
#endif
    api_ = create();
    if (!api_) return false;
    deadline_ = Clock::now() + timeout_;
    api_->RegisterSpi(this);
    api_->SubscribePrivateTopic(THOST_TERT_QUICK);
    api_->SubscribePublicTopic(THOST_TERT_QUICK);
    api_->RegisterFront(front_.data());
    api_->Init();
    return true;
  }

  // SimNowTradingAdapter implementation.
  const TraderLoginStatus& login_status() const override { return status_; }

  // ITradingAdapter implementation. Login cannot certify an account snapshot
  // or transmit orders.
  void SetCallback(ITradingAdapterCallback&) override {}
  bool StartAccountSync(std::uint64_t) override { return false; }
  SendResult SubmitOrder(const SubmitOrderRequest&) override {
    return SendResult::kNotSent;
  }
  SendResult CancelOrder(const AdapterCancelOrderRequest&) override {
    return SendResult::kNotSent;
  }
  void Run() override {
    if (status_.phase == LoginPhase::kFailed) return;
    if (status_.phase != LoginPhase::kLoggedIn && Clock::now() >= deadline_) {
      const auto phase = status_.phase;
      Fail(phase == LoginPhase::kAuthenticating ? "authentication_timeout" :
           phase == LoginPhase::kLoggingIn ? "login_timeout" : "connection_timeout", 0);
      return;
    }
    Event event{};
    {
      std::unique_lock lock(mutex_, std::try_to_lock);
      if (!lock) return;
      if (overflow_) {
        Fail("callback_overflow", 0);
        return;
      }
      if (!size_) return;
      event = queue_[head_];
      head_ = (head_ + 1) % queue_.size();
      --size_;
      if (event.kind != Kind::kDisconnect &&
          event.generation != generation_) return;
    }
    if (event.kind == Kind::kDisconnect) {
      status_ = {};
      status_.phase = LoginPhase::kDisconnected;
      status_.code = event.code;
      deadline_ = Clock::now() + timeout_;
    } else if (event.kind == Kind::kConnect) {
      if (status_.phase != LoginPhase::kConnecting &&
          status_.phase != LoginPhase::kDisconnected) return;
      if (!NextRequest()) return;
      status_ = {};
      status_.phase = LoginPhase::kAuthenticating;
      const int rc = api_->ReqAuthenticate(&auth_, id_);
      if (rc) Fail("auth_request", rc);
    } else {
      Response(event);
    }
  }

 private:
  // CThostFtdcTraderSpi implementation.
  void OnFrontConnected() override { Push({Kind::kConnect}); }
  void OnFrontDisconnected(int code) override {
    Push({Kind::kDisconnect, 0, code});
  }
  void OnRspAuthenticate(CThostFtdcRspAuthenticateField* r,
                         CThostFtdcRspInfoField* error, int id,
                         bool last) override {
    Push({Kind::kAuth, id, error ? error->ErrorID : 0,
          r && Field(r->BrokerID) == broker_ &&
              Field(r->UserID) == user_, last});
  }
  void OnRspUserLogin(CThostFtdcRspUserLoginField* r,
                      CThostFtdcRspInfoField* error, int id,
                      bool last) override {
    Event e{Kind::kLogin, id, error ? error->ErrorID : 0,
            r && Field(r->BrokerID) == broker_ &&
                Field(r->UserID) == user_, last};
    if (r) {
      e.front = r->FrontID;
      e.session = r->SessionID;
      std::copy_n(r->TradingDay, sizeof(e.day), e.day);
      std::copy_n(r->MaxOrderRef, sizeof(e.ref), e.ref);
    }
    Push(e);
  }
  void OnRspError(CThostFtdcRspInfoField* e, int id, bool last) override {
    Push({Kind::kError, id, e ? e->ErrorID : -1, false, last});
  }

  // A disconnect invalidates earlier events still waiting in the queue.
  void Push(const Event& e) {
    std::lock_guard lock(mutex_);
    if (size_ == queue_.size()) {
      overflow_ = true;
    } else {
      if (e.kind == Kind::kDisconnect) ++generation_;
      auto& event = queue_[(head_ + size_++) % queue_.size()];
      event = e;
      event.generation = generation_;
    }
  }
  bool NextRequest() {
    if (id_ == std::numeric_limits<int>::max()) {
      Fail("request_id_exhausted", 0);
      return false;
    }
    ++id_;
    return true;
  }
  void Fail(const char* reason, int code) {
    status_ = {};
    status_.phase = LoginPhase::kFailed;
    status_.reason = reason;
    status_.code = code;
  }
  void Response(const Event& e) {
    const bool auth = status_.phase == LoginPhase::kAuthenticating;
    const auto expected = auth ? Kind::kAuth : Kind::kLogin;
    if ((!auth && status_.phase != LoginPhase::kLoggingIn) || e.id != id_ ||
        (e.kind != Kind::kError && e.kind != expected)) return;
    if (e.code || e.kind == Kind::kError) {
      Fail(auth ? "authentication" : "login", e.code);
      return;
    }
    if (!e.last) return;
    const auto day = Field(e.day);
    if (!e.valid || (!auth && (day.size() != 8 ||
        day.find_first_not_of("0123456789") != day.npos))) {
      Fail("invalid_response", 0);
      return;
    }
    if (auth) {
      if (!NextRequest()) return;
      status_.phase = LoginPhase::kLoggingIn;
#ifdef YQUANT_CTP_MAC_LOGIN
      const int rc = api_->ReqUserLogin(&login_, id_, info_length_, info_);
#else
      const int rc = api_->ReqUserLogin(&login_, id_);
#endif
      if (rc) Fail("login_request", rc);
    } else {
      status_.phase = LoginPhase::kLoggedIn;
      status_.front_id = e.front;
      status_.session_id = e.session;
      status_.trading_day = Field(e.day);
      status_.max_order_ref = Field(e.ref);
    }
  }

  CThostFtdcTraderApi* api_ = nullptr;
#ifdef YQUANT_CTP_MAC_LOGIN
  TThostFtdcClientSystemInfoType info_{};
  int info_length_ = 0;
#endif
  std::string front_;
  const std::string broker_, user_;
  std::chrono::milliseconds timeout_;
  Clock::time_point deadline_;
  TraderLoginStatus status_;
  CThostFtdcReqAuthenticateField auth_{};
  CThostFtdcReqUserLoginField login_{};
  int id_ = 0;
  std::mutex mutex_;
  std::array<Event, 32> queue_{};
  std::size_t head_ = 0, size_ = 0;
  std::uint64_t generation_ = 0;
  bool overflow_ = false;
};

}  // namespace

std::unique_ptr<SimNowTradingAdapter> internal::StartTraderSession(
    const SimNowTraderConfig& config,
    const std::function<CThostFtdcTraderApi*()>& create) {
  auto session = std::make_unique<Session>(config);
  if (!session->Start(config, create)) return nullptr;
  return session;
}

std::unique_ptr<SimNowTradingAdapter> SimNowTradingAdapter::Connect(
    const SimNowTraderConfig& config) {
  return internal::StartTraderSession(config, [] {
    return CThostFtdcTraderApi::CreateFtdcTraderApi();
  });
}

}  // namespace yquant::trading
