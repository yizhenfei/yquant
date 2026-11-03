#include "yquant/trading/simnow_trading_adapter.h"

#include <ThostFtdcTraderApi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <tuple>
#include <utility>
#include <variant>

#include "simnow_cancellations.h"
#include "simnow_contract_query.h"
#include "simnow_fields.h"
#include "simnow_order_holds.h"
#include "simnow_order_tag.h"
#include "simnow_position_costs.h"
#include "simnow_submissions.h"
#include "simnow_trader_session.h"
#include "yquant/trading/trading_service.h"

namespace yquant::trading {
namespace {

using Clock = std::chrono::steady_clock;

using internal::Copy;
using internal::Field;

using internal::NativeId;

enum class Kind {
  kConnect, kDisconnect, kAuth, kLogin, kError, kFunds, kPositions, kDetails, kOrders, kTrades, kTerms,
  kLiveOrder, kLiveTrade, kInsertError, kActionError
};

bool IsControlEvent(Kind kind) {
  return kind == Kind::kConnect || kind == Kind::kDisconnect ||
         kind == Kind::kAuth || kind == Kind::kLogin || kind == Kind::kError;
}

// The exchange order number associates a native trade with its order.
struct HistoryFill {
  TradeId id;
  std::string system_id, symbol;
  char direction, offset, hedge;
  double price;
  int quantity;
};

// Retains identity from an insert error before the exchange assigns an ID.
struct InsertError {
  std::string ref, exchange, symbol;
  std::optional<SimNowOrderOwner> owner;
  int input_id;
};

// Copies just the callback data needed by Run, never SDK pointers or passwords.
struct Event {
  Kind kind;
  int id = 0, code = 0;
  bool valid = false, last = false;
  int front = 0, session = 0;
  TThostFtdcDateType day{};
  TThostFtdcOrderRefType ref{};
  std::uint64_t generation = 0;
  std::unique_ptr<CThostFtdcInvestorPositionDetailField> detail{};
  internal::TermsField field = internal::TermsField::kInstrument;
  std::unique_ptr<internal::TermsRow> terms_row{};
  std::variant<std::monostate, SimNowBalance, SimNowPosition,
               SimNowOrder, HistoryFill, InsertError, internal::ActionError> row{};
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
      : front_(c.front), broker_(c.broker), user_(c.user), timeout_(c.timeout), instruments_(c.instruments), submissions_(instruments_) {}
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

  bool StartSnapshot(std::string_view symbol) override {
    if (status_.phase != LoginPhase::kLoggedIn ||
        (query_.phase != SnapshotPhase::kIdle &&
         query_.phase != SnapshotPhase::kFailed &&
         !(query_.phase == SnapshotPhase::kComplete && !snapshot_.holds_error.empty()))) {
      return false;
    }
    if (symbol.size() >= sizeof(TThostFtdcInstrumentIDType) ||
        symbol.find('\0') != symbol.npos) return false;
    late_system_ids_.clear();
    late_refs_.clear();
    hold_rates_.clear();
    fetching_hold_rate_ = false;
    contract_.reset();
    if (!symbol.empty()) contract_.emplace(symbol);
    position_costs_ = {};
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

  // ITradingAdapter implementation.
  void SetCallback(ITradingAdapterCallback& callback) override { callback_ = &callback; }
  bool StartAccountSync(std::uint64_t id) override {
    if (!callback_ || !id || id <= sync_id_ || status_.phase == LoginPhase::kFailed) {
      return false;
    }
    sync_id_ = id;
    sync_started_ = false;
    staged_ = false;
    return true;
  }
  SendResult SubmitOrder(const SubmitOrderRequest& request) override {
    if (!BeginTradingRequest()) return SendResult::kNotSent;
    CThostFtdcInputOrderField row{};
    Copy(row.BrokerID, broker_);
    Copy(row.InvestorID, user_);
    Copy(row.UserID, user_);
    if (!submissions_.Prepare(request.order, id_, row)) return SendResult::kNotSent;
    const int rc = api_->ReqOrderInsert(&row, id_);
    if (rc == 0) return SendResult::kSent;
    std::fprintf(stderr, "simnow insert_not_sent request=%d code=%d\n", id_, rc);
    return SendResult::kNotSent;
  }
  SendResult CancelOrder(const AdapterCancelOrderRequest& request) override {
    if (!request.service_cancel_id || !BeginTradingRequest()) return SendResult::kNotSent;
    CThostFtdcInputOrderActionField row{};
    if (!submissions_.SetCancelTarget(request.order_id, row) &&
        !SetRecoveredCancelTarget(request.order_id, row)) return SendResult::kNotSent;
    Copy(row.BrokerID, broker_);
    Copy(row.InvestorID, user_);
    Copy(row.UserID, user_);
    cancellations_.Register(request, id_, row);
    const int rc = api_->ReqOrderAction(&row, id_);
    if (rc == 0) return SendResult::kSent;
    cancellations_.NotSent(id_);
    std::fprintf(stderr, "simnow cancel_not_sent request=%d code=%d\n", id_, rc);
    return SendResult::kNotSent;
  }
  void Run() override {
    AdvanceSession();
    if (sync_id_) AdvanceAccountSync();
  }

 private:
  bool BeginTradingRequest() {
    if (!staged_ || status_.phase != LoginPhase::kLoggedIn) return false;
    {
      std::lock_guard lock(mutex_);
      if (overflow_ || generation_ != login_generation_) return false;
    }
    return NextRequest();
  }

  bool SetRecoveredCancelTarget(OrderId id, CThostFtdcInputOrderActionField& r) const {
    const auto* order = snapshot_.history.Find(id);
    if (!order || (order->status != THOST_FTDC_OST_NoTradeQueueing &&
                   order->status != THOST_FTDC_OST_PartTradedQueueing)) return false;
    const auto index = static_cast<std::size_t>(order - snapshot_.history.orders().data());
    const auto late = late_refs_.find(index);
    const auto& system = late == late_refs_.end() ? order->wire_system_id : late->second;
    r.FrontID = order->front;
    r.SessionID = order->session;
    return Copy(r.InstrumentID, order->symbol) && Copy(r.ExchangeID, order->exchange) &&
           (order->ref.empty() || Copy(r.OrderRef, order->wire_ref)) &&
           (system.empty() || Copy(r.OrderSysID, system)) &&
           (order->invest_unit.empty() || Copy(r.InvestUnitID, order->invest_unit));
  }

  void AdvanceAccountSync() {
    if (status_.phase == LoginPhase::kFailed || status_.phase == LoginPhase::kDisconnected) {
      FailAccountAttempt();
      return;
    }
    if (status_.phase != LoginPhase::kLoggedIn || staged_) return;
    if (!sync_started_) {
      if (observation_day_ != status_.trading_day) {
        observation_day_ = status_.trading_day;
        observed_.clear();
      }
      sync_deadline_ = Clock::now() + timeout_;
      BeginWindow();
      sync_started_ = true;
    }
    if (Clock::now() >= sync_deadline_) {
      FailSnapshot("unstable_startup_boundary", 0);
    }
    if (query_.phase == SnapshotPhase::kFailed) {
      FailAccountAttempt();
      return;
    }
    if (query_.phase != SnapshotPhase::kComplete) return;
    // Events still in the SDK handoff queue belong to this comparison. Once
    // the queue is drained, later arrivals are handled against the installed book.
    {
      std::lock_guard lock(mutex_);
      if (!queue_.empty() || overflow_) return;
    }
    if (!CoveredByHistory()) {
      BeginWindow();
      return;
    }
    AccountRecovery baseline;
    const auto error = BuildAccountRecovery(snapshot_, instruments_, baseline);
    if (!error.empty()) {
      std::fprintf(stderr, "simnow recovery_unavailable reason=%s\n", error.c_str());
      FailAccountAttempt();
      return;
    }
    for (const auto& [contract, binding] : instruments_) {
      const auto it = hold_rates_.find({contract.first, contract.second, "", THOST_FTDC_HF_Speculation});
      if (it == hold_rates_.end() || it->second.symbol.empty()) {
        FailAccountAttempt();
        return;
      }
      auto buy = it->second.buy, sell = it->second.sell;
      buy.close_today_first = sell.close_today_first =
          binding.close_priority == SimNowClosePriority::kTodayFirst;
      baseline.order_terms.emplace(binding.instrument_id, std::pair{buy, sell});
    }
    cancellations_.Start(status_.front_id, status_.session_id);
    submissions_.Start(status_.front_id, status_.session_id,
                       status_.max_order_ref, snapshot_.history);
    staged_ = true;
    callback_->OnAccountRecovery(sync_id_, std::move(baseline));
    callback_->OnAccountSyncReport({sync_id_, AccountSyncResult::kComplete});
    before_ = {};
    observed_.clear();
  }
  void BeginWindow() {
    query_.phase = SnapshotPhase::kIdle;
    StartSnapshot({});
    query_deadline_ = sync_deadline_;
    query_.phase = SnapshotPhase::kOrders;
    opening_history_ = true;
    dirty_window_ = false;
    before_ = {};
  }

  bool CoveredByHistory() const {
    if (dirty_window_) return false;
    for (const auto& e : observed_) {
      if (!e.valid || Field(e.day) != snapshot_.trading_day) return false;
      if (const auto* f = std::get_if<HistoryFill>(&e.row)) {
        auto it = snapshot_.history.fills().find(f->id);
        if (it == snapshot_.history.fills().end() || it->second.price != f->price ||
            it->second.quantity != f->quantity) return false;
        const auto& o = snapshot_.history.orders()[it->second.order_index];
        if (o.system_id != f->system_id || o.symbol != f->symbol ||
            o.direction != f->direction || o.offset != f->offset || o.hedge != f->hedge) {
          return false;
        }
      } else if (const auto* o = std::get_if<SimNowOrder>(&e.row)) {
        const auto* match = snapshot_.history.FindNative(*o);
        if (!match || match->symbol != o->symbol || match->exchange != o->exchange ||
            match->quantity != o->quantity || match->direction != o->direction ||
            match->offset != o->offset || match->hedge != o->hedge ||
            match->price != o->price || match->traded < o->traded ||
            match->invest_unit != o->invest_unit || match->price_type != o->price_type ||
            (!o->system_id.empty() && match->system_id != o->system_id) ||
            (o->owner && match->owner != o->owner)) return false;
        if (match->status != o->status &&
            o->status != THOST_FTDC_OST_NoTradeQueueing &&
            o->status != THOST_FTDC_OST_PartTradedQueueing &&
            o->status != THOST_FTDC_OST_Unknown) return false;
      } else {
        return false;
      }
    }
    return true;
  }
  void FailAccountAttempt() {
    if (Collecting()) FailSnapshot("account_sync_interrupted", 0);
    if (staged_) callback_->OnRecoveredEvent(RecoveryRoutingFailure{});
    staged_ = false;
    opening_history_ = false;
    before_ = {};
    callback_->OnAccountSyncReport({std::exchange(sync_id_, 0), AccountSyncResult::kFailed});
  }

  void AdvanceSession() {
    if (status_.phase == LoginPhase::kFailed) return;
    if (status_.phase != LoginPhase::kLoggedIn && Clock::now() >= deadline_) {
      const auto phase = status_.phase;
      Fail(phase == LoginPhase::kAuthenticating ? "authentication_timeout" :
           phase == LoginPhase::kLoggingIn ? "login_timeout" : "connection_timeout", 0);
      return;
    }
    if (Collecting() && Clock::now() >= query_deadline_) {
      if (query_.phase == SnapshotPhase::kContractTerms) {
        FailRate(contract_->field_name(), 0);
      } else {
        FailSnapshot("snapshot_timeout", 0);
      }
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
    } else if (staged_ && event.kind == Kind::kActionError) {
      if (event.valid) cancellations_.Reject(event.id, std::get<internal::ActionError>(event.row),
                                              *callback_);
    } else if (staged_ && event.kind == Kind::kError && cancellations_.HasRequest(event.id)) {
      if (event.valid && event.code) cancellations_.Reject(event.id, {}, *callback_);
    } else if (staged_ && (event.kind == Kind::kInsertError ||
               (event.kind == Kind::kError && submissions_.ByRequest(event.id)))) {
      DeliverInsertError(event);
    } else if (event.kind == Kind::kLiveTrade || event.kind == Kind::kLiveOrder) {
      if (sync_id_ && staged_) DeliverLive(event);
      else if (sync_id_ && sync_started_) {
        dirty_window_ = true;
        observed_.push_back(std::move(event));
      }
    } else if (Collecting()) {
      SnapshotResponse(std::move(event));
    } else {
      Response(event);
    }
  }

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
    Push(std::move(e));
  }
  void OnRspError(CThostFtdcRspInfoField* e, int id, bool last) override {
    Push({Kind::kError, id, e ? e->ErrorID : -1, e != nullptr, last});
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
          r->PositionCost, r->OpenCost, r->UseMargin, r->PreSettlementPrice, {}, {}};
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

  void OnRspQryInvestorPositionDetail(CThostFtdcInvestorPositionDetailField* r,
      CThostFtdcRspInfoField* error, int id, bool last) override {
    Event e{Kind::kDetails, id, error ? error->ErrorID : 0, true, last};
    if (r) {
      e.valid = Field(r->BrokerID) == broker_ && Field(r->InvestorID) == user_;
      e.detail = std::make_unique<CThostFtdcInvestorPositionDetailField>(*r);
    }
    Push(std::move(e));
  }
  void OnRspQryOrder(CThostFtdcOrderField* r, CThostFtdcRspInfoField* error,
                     int id, bool last) override {
    PushOrder({Kind::kOrders, id, error ? error->ErrorID : 0, true, last}, r);
  }
  void OnRspQryTrade(CThostFtdcTradeField* r, CThostFtdcRspInfoField* error,
                     int id, bool last) override {
    PushFill({Kind::kTrades, id, error ? error->ErrorID : 0, true, last}, r);
  }
  void OnRspOrderAction(CThostFtdcInputOrderActionField* row, CThostFtdcRspInfoField* error,
                        int id, bool last) override {
    PushActionError(row, error, id, last);
  }
  void OnErrRtnOrderAction(CThostFtdcOrderActionField* row,
                           CThostFtdcRspInfoField* error) override {
    PushActionError(row, error, 0, true);
  }
  void OnRspOrderInsert(CThostFtdcInputOrderField* row, CThostFtdcRspInfoField* error,
                        int id, bool last) override {
    PushInsertError(row, error, id, last);
  }
  void OnErrRtnOrderInsert(CThostFtdcInputOrderField* row,
                           CThostFtdcRspInfoField* error) override {
    PushInsertError(row, error, 0, true);
  }
  void OnRtnTrade(CThostFtdcTradeField* row) override {
    PushFill({Kind::kLiveTrade}, row);
  }
  void OnRtnOrder(CThostFtdcOrderField* row) override {
    PushOrder({Kind::kLiveOrder}, row);
  }

  void OnRspQryInstrument(CThostFtdcInstrumentField* r,
                          CThostFtdcRspInfoField* error, int id, bool last) override {
    PushTerms(internal::TermsField::kInstrument, r, error, id, last);
  }
  void OnRspQryProduct(CThostFtdcProductField* r, CThostFtdcRspInfoField* error,
                       int id, bool last) override {
    PushTerms(internal::TermsField::kProduct, r, error, id, last);
  }
  void OnRspQryInstrumentMarginRate(CThostFtdcInstrumentMarginRateField* r,
      CThostFtdcRspInfoField* error, int id, bool last) override {
    PushTerms(internal::TermsField::kMargin, r, error, id, last,
              !r || MatchesInvestor(*r));
  }
  void OnRspQryExchangeMarginRate(CThostFtdcExchangeMarginRateField* r,
      CThostFtdcRspInfoField* error, int id, bool last) override {
    PushTerms(internal::TermsField::kExchangeMargin, r, error, id, last,
              !r || Field(r->BrokerID) == broker_);
  }
  void OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField* r,
      CThostFtdcRspInfoField* error, int id, bool last) override {
    PushTerms(internal::TermsField::kFees, r, error, id, last,
              !r || MatchesInvestor(*r));
  }

  template <typename Row>
  void PushActionError(const Row* row, const CThostFtdcRspInfoField* error, int id, bool last) {
    if (!error || !error->ErrorID) return;
    Event e{Kind::kActionError, id, error->ErrorID, true, last};
    internal::ActionError action;
    if (row) {
      e.valid = Field(row->BrokerID) == broker_ && Field(row->InvestorID) == user_ &&
                (row->ActionFlag == THOST_FTDC_AF_Delete || (id && !row->ActionFlag));
      action = {row->RequestID, row->OrderActionRef, row->FrontID, row->SessionID,
                NativeId(row->OrderRef), std::string(Field(row->ExchangeID)),
                NativeId(row->OrderSysID), std::string(Field(row->InstrumentID)),
                std::string(Field(row->OrderMemo))};
    }
    e.row = std::move(action);
    Push(std::move(e));
  }

  void PushInsertError(const CThostFtdcInputOrderField* row,
                       const CThostFtdcRspInfoField* error, int id, bool last) {
    if (!error || !error->ErrorID) return;
    Event e{Kind::kInsertError, id, error->ErrorID, true, last};
    if (row) {
      e.valid = Field(row->BrokerID) == broker_ && Field(row->InvestorID) == user_;
      e.row = InsertError{NativeId(row->OrderRef), std::string(Field(row->ExchangeID)),
          std::string(Field(row->InstrumentID)), internal::ReadOrderTag(*row), row->RequestID};
    }
    Push(std::move(e));
  }

  void DeliverInsertError(const Event& e) {
    if (!e.code || !e.valid) return;
    const auto* entry = submissions_.ByRequest(e.id);
    if (const auto* error = std::get_if<InsertError>(&e.row)) {
      const auto* by_ref = submissions_.ByReference(error->ref);
      if (e.id == 0 && error->owner) entry = by_ref;
      if (!entry || (!error->ref.empty() && entry != by_ref) ||
          (error->input_id && error->input_id != entry->request_id) ||
          (!error->symbol.empty() &&
           !submissions_.MatchesInstrument(*entry, error->exchange, error->symbol)) ||
          (error->owner && (error->owner->order_id != entry->order.order_id ||
                            error->owner->strategy_id != entry->order.strategy_id))) {
        std::fprintf(stderr, "simnow unassociated_insert_error request=%d code=%d\n", e.id, e.code);
        return;
      }
    }
    if (entry) callback_->OnOrderReport({entry->order.order_id, AdapterOrderState::kRejected});
  }

  void PushOrder(Event e, CThostFtdcOrderField* r) {
    if (r) {
      e.valid = MatchesHistoryRow(*r, e);
      e.row = SimNowOrder{r->FrontID, r->SessionID, NativeId(r->OrderRef),
          std::string(Field(r->ExchangeID)), NativeId(r->OrderSysID),
          std::string(Field(r->InstrumentID)), r->Direction, r->CombOffsetFlag[0],
          r->CombHedgeFlag[0], r->OrderStatus, r->OrderSubmitStatus,
          r->OrderPriceType, r->LimitPrice, r->VolumeTotalOriginal, r->VolumeTraded,
          r->VolumeTotal, internal::ReadOrderTag(*r), std::string(Field(r->InvestUnitID)),
          r->IsSwapOrder != 0 || Field(r->CombOffsetFlag).size() > 1 ||
              Field(r->CombHedgeFlag).size() > 1,
          Field(r->CurrencyID).empty() ? std::nullopt :
              std::optional{model::Currency(Field(r->CurrencyID))},
          std::string(Field(r->OrderRef)), std::string(Field(r->OrderSysID))};
    }
    Push(std::move(e));
  }
  void PushFill(Event e, CThostFtdcTradeField* r) {
    if (r) {
      e.valid = MatchesHistoryRow(*r, e);
      // Opposite sides of a self-match can share the exchange's trade number.
      TradeId trade{std::string(Field(r->ExchangeID)),
                    std::string(Field(r->TradingDay)) + "/" + r->Direction,
                    NativeId(r->TradeID)};
      e.row = HistoryFill{std::move(trade), NativeId(r->OrderSysID),
          std::string(Field(r->InstrumentID)), r->Direction, r->OffsetFlag,
          r->HedgeFlag, r->Price, r->Volume};
    }
    Push(std::move(e));
  }

  const SimNowOrder* FindSystemOrder(const std::string& exchange,
                                     const std::string& system_id) const {
    const auto& history = snapshot_.history;
    if (const auto* order = history.Find(exchange, system_id)) return order;
    auto it = late_system_ids_.find({exchange, system_id});
    return it == late_system_ids_.end() ? nullptr : &history.orders()[it->second];
  }
  // A queued order may acquire its exchange ID only after the startup query.
  // Keep that alias separate so the published query snapshot stays immutable.
  const SimNowOrder* ResolveOrder(const SimNowOrder& o) {
    const auto& history = snapshot_.history;
    const auto* by_ref = history.Find(o.front, o.session, o.ref);
    const auto* by_system = FindSystemOrder(o.exchange, o.system_id);
    if (by_ref && by_system && by_ref != by_system) return nullptr;
    const auto* original = by_ref ? by_ref : by_system;
    if (!original || original->symbol != o.symbol || original->exchange != o.exchange ||
        original->quantity != o.quantity || original->direction != o.direction ||
        original->offset != o.offset || original->hedge != o.hedge) return nullptr;
    if (!o.system_id.empty() && !by_system) {
      if (!original->system_id.empty()) return nullptr;
      const auto index = static_cast<std::size_t>(original - history.orders().data());
      if (!late_refs_.emplace(index, o.wire_system_id).second) return nullptr;
      late_system_ids_.emplace(std::pair{o.exchange, o.system_id}, index);
    }
    return original;
  }
  bool CompleteCancellations(OrderId id, int quantity, int filled) {
    if (filled < 0 || filled > quantity) {
      callback_->OnRecoveredEvent(RecoveryRoutingFailure{});
      return true;
    }
    return cancellations_.Complete(id, filled, *callback_);
  }

  bool DeliverSubmittedOrder(const SimNowOrder& update) {
    bool conflict = false;
    const auto* entry = submissions_.Match(update,
        FindSystemOrder(update.exchange, update.system_id) != nullptr, conflict);
    if (!entry) {
      if (conflict) callback_->OnRecoveredEvent(RecoveryRoutingFailure{});
      return conflict;
    }
    AdapterOrderState state;
    if (update.submit_status == THOST_FTDC_OSS_InsertRejected) {
      state = AdapterOrderState::kRejected;
    } else {
      switch (update.status) {
        case THOST_FTDC_OST_Canceled:
        case THOST_FTDC_OST_NoTradeNotQueueing:
        case THOST_FTDC_OST_PartTradedNotQueueing:
          state = AdapterOrderState::kCanceled;
          break;
        case THOST_FTDC_OST_NoTradeQueueing:
        case THOST_FTDC_OST_PartTradedQueueing:
        case THOST_FTDC_OST_AllTraded:
          state = AdapterOrderState::kAccepted;
          break;
        default: return true;  // Still waiting for a definitive order state.
      }
    }
    if (state == AdapterOrderState::kCanceled &&
        CompleteCancellations(entry->order.order_id, entry->order.quantity, update.traded)) return true;
    callback_->OnOrderReport({entry->order.order_id, state, update.traded});
    return true;
  }

  void DeliverLive(const Event& e) {
    if (!e.valid) return;
    if (Field(e.day) != snapshot_.trading_day) {
      const auto* order = std::get_if<SimNowOrder>(&e.row);
      if (Field(e.day).empty() && order && order->submit_status == THOST_FTDC_OSS_InsertRejected &&
          order->front == status_.front_id && order->session == status_.session_id) {
        DeliverSubmittedOrder(*order);
      }
      return;
    }
    const auto& history = snapshot_.history;
    const auto* fill = std::get_if<HistoryFill>(&e.row);
    const auto* update = std::get_if<SimNowOrder>(&e.row);
    const SimNowOrder* original = nullptr;
    if (fill) {
      if (const auto* entry = submissions_.ByExchange(fill->id.venue, fill->system_id)) {
        if (submissions_.MatchesInstrument(*entry, fill->id.venue, fill->symbol) &&
            submissions_.MatchesTrade(*entry, fill->direction, fill->offset, fill->hedge)) {
          callback_->OnTradeReport({entry->order.order_id, fill->id, fill->price, fill->quantity});
        } else {
          callback_->OnRecoveredEvent(RecoveryRoutingFailure{});
        }
        return;
      }
      original = FindSystemOrder(fill->id.venue, fill->system_id);
      if (original && (original->symbol != fill->symbol ||
          original->direction != fill->direction || original->hedge != fill->hedge ||
          original->offset != fill->offset)) original = nullptr;
    } else if (update) {
      if (DeliverSubmittedOrder(*update)) return;
      original = ResolveOrder(*update);
    }
    if (!original) {
      callback_->OnRecoveredEvent(RecoveryRoutingFailure{});
      return;
    }
    if (original->status != THOST_FTDC_OST_NoTradeQueueing &&
        original->status != THOST_FTDC_OST_PartTradedQueueing) {
      if (fill) {
        const auto old = history.fills().find(fill->id);
        if (old == history.fills().end() || old->second.price != fill->price ||
            old->second.quantity != fill->quantity ||
            &history.orders()[old->second.order_index] != original) {
          callback_->OnRecoveredEvent(RecoveryRoutingFailure{});
        }
      }
      return;
    }
    const auto key = static_cast<std::size_t>(original - history.orders().data());
    if (fill) {
      callback_->OnRecoveredEvent(RecoveredFill{key, fill->id, fill->price, fill->quantity});
    } else if (update->status == THOST_FTDC_OST_Canceled ||
               update->status == THOST_FTDC_OST_PartTradedNotQueueing ||
               update->status == THOST_FTDC_OST_NoTradeNotQueueing) {
      if (!original->owner || !CompleteCancellations(original->owner->order_id,
                                                       original->quantity, update->traded)) {
        callback_->OnRecoveredEvent(RecoveredCancellation{key, update->traded});
      }
    } else if (update->status == THOST_FTDC_OST_PartTradedQueueing ||
               update->status == THOST_FTDC_OST_AllTraded) {
      callback_->OnRecoveredEvent(RecoveredCumulative{key, update->traded});
    }
  }

  template <typename Row>
  bool MatchesHistoryRow(const Row& row, Event& event) const {
    std::copy_n(row.TradingDay, sizeof(event.day), event.day);
    return Field(row.BrokerID) == broker_ && Field(row.InvestorID) == user_ &&
           (row.Direction == THOST_FTDC_D_Buy || row.Direction == THOST_FTDC_D_Sell);
  }

  template <typename Row>
  bool MatchesInvestor(const Row& row) const {
    if (Field(row.BrokerID) != broker_) {
      return false;
    }
    return (row.InvestorRange == THOST_FTDC_IR_Single &&
            Field(row.InvestorID) == user_) ||
           (row.InvestorRange == THOST_FTDC_IR_All &&
            (Field(row.InvestorID).empty() || Field(row.InvestorID) == user_));
  }
  template <typename Row>
  void PushTerms(internal::TermsField field, const Row* row,
                 CThostFtdcRspInfoField* error, int id, bool last,
                 bool valid = true) {
    Event e{Kind::kTerms, id, error ? error->ErrorID : 0, valid, last};
    e.field = field;
    if (row) e.terms_row = std::make_unique<internal::TermsRow>(*row);
    Push(std::move(e));
  }

  bool Collecting() const {
    return query_.phase == SnapshotPhase::kFunds ||
           query_.phase == SnapshotPhase::kPositions ||
           query_.phase == SnapshotPhase::kPositionMultiplier ||
           query_.phase == SnapshotPhase::kPositionDetails ||
           query_.phase == SnapshotPhase::kOrders ||
           query_.phase == SnapshotPhase::kTrades ||
           query_.phase == SnapshotPhase::kContractTerms;
  }
  void FailSnapshot(const char* reason, int code) {
    query_ = {SnapshotPhase::kFailed, reason, code};
    position_costs_ = {};
    snapshot_ = {};
  }
  void SendQuery() {
    if (!NextRequest()) return;
    const bool funds = query_.phase == SnapshotPhase::kFunds;
    int rc;
    const bool terms = query_.phase == SnapshotPhase::kContractTerms;
    const char* failure = "positions_request";
    if (query_.phase == SnapshotPhase::kPositionMultiplier) {
      CThostFtdcQryInstrumentField r{};
      if (!Copy(r.ExchangeID, position_costs_.contract().exchange) ||
          !Copy(r.InstrumentID, position_costs_.contract().symbol)) {
        FailSnapshot("invalid_position_contract", 0);
        return;
      }
      rc = api_->ReqQryInstrument(&r, id_);
      failure = "position_multiplier_request";
    } else if (query_.phase == SnapshotPhase::kPositionDetails) {
      CThostFtdcQryInvestorPositionDetailField r{};
      Copy(r.BrokerID, broker_);
      Copy(r.InvestorID, user_);
      Copy(r.ExchangeID, position_costs_.contract().exchange);
      Copy(r.InstrumentID, position_costs_.contract().symbol);
      rc = api_->ReqQryInvestorPositionDetail(&r, id_);
      failure = "position_details_request";
    } else if (query_.phase == SnapshotPhase::kOrders) {
      CThostFtdcQryOrderField r{};
      Copy(r.BrokerID, broker_);
      Copy(r.InvestorID, user_);
      rc = api_->ReqQryOrder(&r, id_);
      failure = "orders_request";
    } else if (query_.phase == SnapshotPhase::kTrades) {
      CThostFtdcQryTradeField r{};
      Copy(r.BrokerID, broker_);
      Copy(r.InvestorID, user_);
      rc = api_->ReqQryTrade(&r, id_);
      failure = "trades_request";
    } else if (terms) {
      failure = contract_->field_name();
      rc = contract_->Send(*api_, broker_, user_, id_);
    } else if (funds) {
      failure = "funds_request";
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
      if (terms) FailRate(failure, rc);
      else FailSnapshot(failure, rc);
    } else {
      query_sent_ = true;
    }
  }
  bool ValidSnapshotRow(const Event& e) const {
    if (e.kind == Kind::kError || e.code || !e.valid) return false;
    if (std::holds_alternative<std::monostate>(e.row)) return e.last;
    return Field(e.day) == snapshot_.trading_day;
  }
  void ReceiveHistory(Event e) {
    auto& history = opening_history_ ? before_ : snapshot_.history;
    if (auto* order = std::get_if<SimNowOrder>(&e.row)) {
      if (!history.AddOrder(std::move(*order))) {
        if (sync_started_ && sync_id_) BeginWindow();
        else FailSnapshot("invalid_order_history", 0);
        return;
      }
    } else if (auto* fill = std::get_if<HistoryFill>(&e.row)) {
      if (!history.AddFill(std::move(fill->id), fill->system_id,
              fill->symbol, fill->direction, fill->offset, fill->hedge,
              fill->price, fill->quantity)) {
        if (sync_started_ && sync_id_) BeginWindow();
        else FailSnapshot("invalid_trade_history", 0);
        return;
      }
    }
    if (!e.last) return;
    if (e.kind == Kind::kOrders) {
      query_.phase = SnapshotPhase::kTrades;
    } else if (opening_history_) {
      opening_history_ = false;
      query_.phase = SnapshotPhase::kFunds;
    } else if (sync_id_ && sync_started_ &&
               (dirty_window_ || !before_.Matches(history))) {
      BeginWindow();
    } else if (sync_id_ && sync_started_ && !history.HasCompleteFills()) {
      FailSnapshot("incomplete_startup_trade_history", 0);
    } else {
      snapshot_.holds_error = internal::CollectOrderRates(snapshot_.history, hold_rates_);
      if (sync_id_ && sync_started_) {
        for (const auto& entry : instruments_) {
          const auto& [exchange, symbol] = entry.first;
          hold_rates_.try_emplace({exchange, symbol, "", THOST_FTDC_HF_Speculation});
        }
      }
      hold_rate_ = hold_rates_.begin();
      if (contract_) query_.phase = SnapshotPhase::kContractTerms;
      else NextHoldRate();
    }
    query_sent_ = false;
  }

  void NextHoldRate() {
    if (snapshot_.holds_error.empty()) {
      while (hold_rate_ != hold_rates_.end() && !hold_rate_->second.symbol.empty()) {
        ++hold_rate_;
      }
      if (hold_rate_ != hold_rates_.end()) {
        const auto& [exchange, symbol, unit, hedge] = hold_rate_->first;
        contract_.emplace(symbol, exchange, unit, hedge);
        fetching_hold_rate_ = true;
        query_sent_ = false;
        query_.phase = SnapshotPhase::kContractTerms;
        return;
      }
      internal::ReconcileOrderHolds(snapshot_, hold_rates_);
    }
    fetching_hold_rate_ = false;
    query_.phase = SnapshotPhase::kComplete;
  }
  void FailRate(const char* reason, int code) {
    if (!fetching_hold_rate_) {
      FailSnapshot(reason, code);
      return;
    }
    snapshot_.holds_error = "missing_order_rates " + contract_->terms.symbol +
                            " " + reason + " code=" + std::to_string(code);
    fetching_hold_rate_ = false;
    query_.phase = SnapshotPhase::kComplete;
  }
  void FinishRate() {
    if (fetching_hold_rate_) {
      hold_rate_->second = std::move(contract_->terms);
      ++hold_rate_;
    } else {
      snapshot_.terms = std::move(contract_->terms);
      const auto matching = hold_rates_.find({contract_->exchange(),
          snapshot_.terms->symbol, "", THOST_FTDC_HF_Speculation});
      if (matching != hold_rates_.end()) matching->second = *snapshot_.terms;
    }
    NextHoldRate();
  }

  void ReceivePositionCosts(const Event& e) {
    const bool multiplier = query_.phase == SnapshotPhase::kPositionMultiplier;
    if (e.kind != Kind::kError) {
      if (multiplier && (e.kind != Kind::kTerms ||
          e.field != internal::TermsField::kInstrument)) return;
      if (!multiplier && e.kind != Kind::kDetails) return;
    }
    if (e.kind == Kind::kError || e.code || !e.valid) {
      FailSnapshot(multiplier ? "position_multiplier_response" :
                                "position_details_response", e.code);
      return;
    }
    if (multiplier) {
      if (e.terms_row && !position_costs_.SetMultiplier(
              std::get<CThostFtdcInstrumentField>(*e.terms_row))) {
        FailSnapshot("invalid_position_multiplier", 0);
        return;
      }
      if (!e.terms_row && (!e.last || !position_costs_.has_multiplier())) {
        FailSnapshot("missing_position_multiplier", 0);
        return;
      }
      if (!e.last) return;
      query_.phase = SnapshotPhase::kPositionDetails;
    } else {
      if (e.detail) {
        if (Field(e.detail->TradingDay) != snapshot_.trading_day) {
          FailSnapshot("position_detail_trading_day", 0);
          return;
        }
        if (const char* error = position_costs_.Add(*e.detail, snapshot_.trading_day)) {
          FailSnapshot(error, 0);
          return;
        }
      } else if (!e.last) {
        FailSnapshot("empty_position_detail", 0);
        return;
      }
      if (!e.last) return;
      if (const char* error = position_costs_.FinishContract()) {
        FailSnapshot(error, 0);
        return;
      }
      query_.phase = position_costs_.complete() ? SnapshotPhase::kOrders
                                               : SnapshotPhase::kPositionMultiplier;
    }
    query_sent_ = false;
  }

  void SnapshotResponse(Event e) {
    if (!query_sent_ || e.id != id_) return;
    if (query_.phase == SnapshotPhase::kPositionMultiplier ||
        query_.phase == SnapshotPhase::kPositionDetails) {
      ReceivePositionCosts(e);
      return;
    }
    if (query_.phase == SnapshotPhase::kContractTerms) {
      if (e.kind != Kind::kError &&
          (e.kind != Kind::kTerms || e.field != contract_->field)) return;
      if (e.kind == Kind::kError || e.code || !e.valid ||
          !contract_->Receive(e.terms_row.get(), e.last)) {
        FailRate(contract_->field_name(), e.code);
      } else if (contract_->complete) {
        FinishRate();
      } else if (e.last) {
        query_sent_ = false;
      }
      return;
    }
    const bool orders = query_.phase == SnapshotPhase::kOrders;
    if (orders || query_.phase == SnapshotPhase::kTrades) {
      if (e.kind != (orders ? Kind::kOrders : Kind::kTrades) &&
          e.kind != Kind::kError) return;
      if (!ValidSnapshotRow(e)) {
        FailSnapshot(orders ? "orders_response" : "trades_response", e.code);
        return;
      }
      ReceiveHistory(std::move(e));
      return;
    }
    const bool funds = query_.phase == SnapshotPhase::kFunds;
    if (e.kind != (funds ? Kind::kFunds : Kind::kPositions) &&
        e.kind != Kind::kError) return;
    if (!ValidSnapshotRow(e)) {
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
    if (const char* error = position_costs_.Prepare(rows)) {
      FailSnapshot(error, 0);
      return;
    }
    query_.phase = position_costs_.complete() ? SnapshotPhase::kOrders
                                             : SnapshotPhase::kPositionMultiplier;
    query_sent_ = false;
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
      login_generation_ = e.generation;
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
  const SimNowInstruments instruments_;
  internal::SimNowSubmissions submissions_;
  internal::SimNowCancellations cancellations_;
  std::uint64_t login_generation_ = 0;
  std::map<std::pair<std::string, std::string>, std::size_t> late_system_ids_;
  // Original SDK spelling of exchange IDs learned after startup, by snapshot row.
  std::unordered_map<std::size_t, std::string> late_refs_;
  ITradingAdapterCallback* callback_ = nullptr;
  std::uint64_t sync_id_ = 0;
  bool sync_started_ = false, staged_ = false;
  bool opening_history_ = false, dirty_window_ = false;
  Clock::time_point sync_deadline_;
  SimNowOrderHistory before_;
  std::vector<Event> observed_;
  std::string observation_day_;
  SnapshotStatus query_;
  internal::SimNowPositionCosts position_costs_;
  std::optional<internal::SimNowContractQuery> contract_;
  internal::OrderRates hold_rates_;
  internal::OrderRates::iterator hold_rate_;
  bool fetching_hold_rate_ = false;
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
