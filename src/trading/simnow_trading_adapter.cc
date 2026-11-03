#include "yquant/trading/simnow_trading_adapter.h"

#include <ThostFtdcTraderApi.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <tuple>
#include <utility>
#include <variant>

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

enum class Kind {
  kConnect, kDisconnect, kAuth, kLogin, kError, kFunds, kPositions
};

bool IsControlEvent(Kind kind) {
  return kind != Kind::kFunds && kind != Kind::kPositions;
}

// Copies just the callback data needed by Run, never SDK pointers or passwords.
struct Event {
  Kind kind;
  int id = 0, code = 0;
  bool valid = false, last = false;
  int front = 0, session = 0;
  TThostFtdcDateType day{};
  TThostFtdcOrderRefType ref{};
  std::uint64_t generation = 0;
  std::variant<std::monostate, SimNowBalance, SimNowPosition> row{};
};

// CTP uses the largest double as an unset value.
bool Amount(double value) {
  return std::isfinite(value) && value != std::numeric_limits<double>::max();
}

bool Nonnegative(double value) { return Amount(value) && value >= 0; }

auto PositionIdentity(const SimNowPosition& row) {
  return std::tie(row.exchange, row.symbol, row.invest_unit, row.direction,
                  row.hedge, row.date);
}

// Serializes login events and startup query responses onto the business thread. Each Run handles
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

  bool StartSnapshot() override {
    if (status_.phase != LoginPhase::kLoggedIn ||
        (query_.phase != SnapshotPhase::kIdle &&
         query_.phase != SnapshotPhase::kFailed)) return false;
    snapshot_ = {};
    snapshot_.trading_day = status_.trading_day;
    query_ = {SnapshotPhase::kFunds};
    query_sent_ = false;
    next_query_ = Clock::now();
    query_deadline_ = next_query_ + timeout_;
    return true;
  }
  SnapshotStatus snapshot_status() const override { return query_; }
  const SimNowStartupSnapshot* snapshot() const override {
    return query_.phase == SnapshotPhase::kComplete ? &snapshot_ : nullptr;
  }

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
    if (Collecting() && Clock::now() >= query_deadline_) {
      FailSnapshot("snapshot_timeout", 0);
    }
    Event event{};
    {
      std::unique_lock lock(mutex_, std::try_to_lock);
      if (!lock) return;
      if (overflow_) {
        Fail("callback_overflow", 0);
        return;
      }
      if (queue_.empty()) {
        lock.unlock();
        if (Collecting() && !query_sent_ && Clock::now() >= next_query_) {
          SendQuery();
        }
        return;
      }
      event = std::move(queue_.front());
      queue_.pop_front();
      if (IsControlEvent(event.kind)) --control_events_;
      if (event.kind != Kind::kDisconnect &&
          event.generation != generation_) return;
    }
    if (event.kind == Kind::kDisconnect) {
      status_ = {};
      if (query_.phase != SnapshotPhase::kIdle) {
        FailSnapshot("snapshot_disconnected", event.code);
      }
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
    } else if (Collecting()) {
      SnapshotResponse(std::move(event));
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

  void OnRspQryTradingAccount(CThostFtdcTradingAccountField* r,
                              CThostFtdcRspInfoField* error, int id,
                              bool last) override {
    Event e{Kind::kFunds, id, error ? error->ErrorID : 0, true, last};
    if (r) {
      SimNowBalance row{model::Currency(Field(r->CurrencyID)), r->Balance,
          r->Available, r->PositionProfit, r->CurrMargin, r->FrozenMargin,
          r->FrozenCash, r->FrozenCommission};
      e.valid = Field(r->BrokerID) == broker_ && Field(r->AccountID) == user_ &&
          row.currency.valid() && Amount(row.balance) && Amount(row.available) &&
          Amount(row.position_profit) && Nonnegative(row.margin) &&
          Nonnegative(row.frozen_margin) && Nonnegative(row.frozen_cash) &&
          Nonnegative(row.frozen_commission);
      std::copy_n(r->TradingDay, sizeof(e.day), e.day);
      e.row = row;
    }
    Push(std::move(e));
  }
  void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* r,
                                CThostFtdcRspInfoField* error, int id,
                                bool last) override {
    Event e{Kind::kPositions, id, error ? error->ErrorID : 0, true, last};
    if (r) {
      SimNowPosition row{std::string(Field(r->ExchangeID)),
          std::string(Field(r->InstrumentID)), std::string(Field(r->InvestUnitID)),
          r->PosiDirection, r->HedgeFlag, r->PositionDate, r->Position,
          r->TodayPosition, r->YdPosition, r->LongFrozen, r->ShortFrozen,
          r->PositionCost, r->OpenCost, r->UseMargin, r->PreSettlementPrice};
      e.valid = Field(r->BrokerID) == broker_ && Field(r->InvestorID) == user_ &&
          !row.exchange.empty() && !row.symbol.empty() &&
          (row.direction == THOST_FTDC_PD_Long ||
           row.direction == THOST_FTDC_PD_Short ||
           row.direction == THOST_FTDC_PD_Net) &&
          (row.date == THOST_FTDC_PSD_Today || row.date == THOST_FTDC_PSD_History) &&
          row.hedge != 0 && row.today >= 0 && row.today <= row.quantity &&
          row.previous_day_quantity >= 0 && row.long_frozen >= 0 &&
          row.short_frozen >= 0 && Amount(row.position_cost) &&
          Amount(row.open_cost) && Nonnegative(row.margin) &&
          Amount(row.previous_settlement);
      std::copy_n(r->TradingDay, sizeof(e.day), e.day);
      e.row = std::move(row);
    }
    Push(std::move(e));
  }

  bool Collecting() const {
    return query_.phase == SnapshotPhase::kFunds ||
           query_.phase == SnapshotPhase::kPositions;
  }
  void FailSnapshot(const char* reason, int code) {
    query_ = {SnapshotPhase::kFailed, reason, code};
    snapshot_ = {};
  }
  void SendQuery() {
    if (!NextRequest()) return;
    const bool funds = query_.phase == SnapshotPhase::kFunds;
    int rc;
    if (funds) {
      CThostFtdcQryTradingAccountField r{};
      Copy(r.BrokerID, broker_);
      Copy(r.InvestorID, user_);
      rc = api_->ReqQryTradingAccount(&r, id_);
    } else {
      CThostFtdcQryInvestorPositionField r{};
      Copy(r.BrokerID, broker_);
      Copy(r.InvestorID, user_);
      rc = api_->ReqQryInvestorPosition(&r, id_);
    }
    if (rc == -2 || rc == -3) {
      next_query_ = Clock::now() + std::chrono::seconds(1);
    } else if (rc != 0) {
      FailSnapshot(funds ? "funds_request" : "positions_request", rc);
    } else {
      query_sent_ = true;
    }
  }
  void SnapshotResponse(Event e) {
    const bool funds = query_.phase == SnapshotPhase::kFunds;
    if (!query_sent_ || e.id != id_ ||
        (e.kind != (funds ? Kind::kFunds : Kind::kPositions) &&
         e.kind != Kind::kError)) return;
    const bool empty = std::holds_alternative<std::monostate>(e.row);
    if (e.kind == Kind::kError || e.code || !e.valid ||
        (!empty && Field(e.day) != snapshot_.trading_day) ||
        (empty && !e.last)) {
      FailSnapshot(funds ? "funds_response" : "positions_response", e.code);
      return;
    }
    if (auto* row = std::get_if<SimNowBalance>(&e.row)) {
      for (const auto& previous : snapshot_.balances) {
        if (previous.currency == row->currency) {
          FailSnapshot("duplicate_currency", 0);
          return;
        }
      }
      snapshot_.balances.push_back(*row);
    } else if (auto* row = std::get_if<SimNowPosition>(&e.row)) {
      snapshot_.positions.push_back(std::move(*row));
    }
    if (!e.last) return;
    if (funds) {
      if (snapshot_.balances.empty()) {
        FailSnapshot("missing_funds", 0);
        return;
      }
      query_.phase = SnapshotPhase::kPositions;
      query_sent_ = false;
      return;
    }
    auto& rows = snapshot_.positions;
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
      return PositionIdentity(a) < PositionIdentity(b);
    });
    if (std::adjacent_find(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
          return PositionIdentity(a) == PositionIdentity(b);
        }) != rows.end()) {
      FailSnapshot("duplicate_position", 0);
      return;
    }
    query_.phase = SnapshotPhase::kComplete;
  }

  // A disconnect invalidates earlier events still waiting in the queue.
  void Push(Event e) {
    std::lock_guard lock(mutex_);
    if (overflow_) return;
    if (IsControlEvent(e.kind) && control_events_ >= 32) {
      overflow_ = true;
    } else {
      if (e.kind == Kind::kDisconnect) ++generation_;
      if (IsControlEvent(e.kind)) ++control_events_;
      e.generation = generation_;
      queue_.push_back(std::move(e));
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
    if (query_.phase != SnapshotPhase::kIdle) FailSnapshot(reason, code);
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
  std::deque<Event> queue_;
  std::size_t control_events_ = 0;
  SnapshotStatus query_;
  SimNowStartupSnapshot snapshot_;
  bool query_sent_ = false;
  Clock::time_point next_query_, query_deadline_;
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
