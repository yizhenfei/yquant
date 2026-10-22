#include "strategyd/strategy_runner.h"

#include <cstdio>
#include <limits>

#include "strategyd/strategy_config_loader.h"
#include "yquant/trading/trading_client_config.h"

namespace yquant::strategyd {

StrategyRunner::StrategyRunner(const base::MonotonicClock& clock,
                               base::EventLoopEnvironment& environment,
                               MarketDataSource& source,
                               trading::TradingService& trading) noexcept
    : clock_(clock),
      environment_(environment),
      source_(source),
      trading_service_(trading),
      market_data_(clock),
      context_(clock, market_data_, trading, timer_) {}

StrategyRunner::~StrategyRunner() noexcept {
  // Run performs orderly drain and exposes cleanup errors. Destruction still
  // releases resources if the caller exits early, without starting another
  // loop.
  Finalize();
}

void StrategyRunner::ResetError() noexcept {
  result_ = {};
  error_stage_ = "";
  error_detail_[0] = '\0';
}

void StrategyRunner::SetErrorIfNotOK(base::Status status, const char* stage,
                                     const char* detail) noexcept {
  if (status.ok() || !result_.ok()) {
    return;
  }
  result_ = status;
  error_stage_ = stage;
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
}

void StrategyRunner::SetQuoteError(base::Status status, const char* stage,
                                   const char* detail,
                                   const model::QuoteTick& tick) noexcept {
  if (status.ok() || !result_.ok()) {
    return;
  }
  result_ = status;
  error_stage_ = stage;
  std::snprintf(error_detail_, sizeof(error_detail_),
                "source_id=%u instrument_id=%llu sequence=%llu: %s",
                tick.source_id.value,
                static_cast<unsigned long long>(tick.instrument_id.value),
                static_cast<unsigned long long>(tick.sequence), detail);
}

base::Status StrategyRunner::LoadAndStart(
    StrategyStartOptions options) noexcept {
  ResetError();
  if (state_ != RunnerState::kIdle) {
    SetErrorIfNotOK({base::ErrorCode::kInvalidState, {}}, "command",
                    "a strategy can only be loaded in the idle state");
    return result_;
  }

  state_ = RunnerState::kStarting;
  auto& config_loader = options.config;
  strategy_id_ = config_loader.strategy_id();
  // Mark cleanup due before calling Prepare because even a failed attempt may
  // own partial resources that require Stop.
  source_prepared_ = true;
  const auto prepared = source_.Prepare(options.market_data);
  if (!prepared.ok()) {
    SetErrorIfNotOK(prepared, "market_data_prepare", source_.error_detail());
    Finalize();
    return result_;
  }
  const auto configured = market_data_.Configure(options.market_data);
  if (!configured.ok()) {
    SetErrorIfNotOK(configured, "market_data_prepare",
                    "could not prepare the instrument dictionary");
    Finalize();
    return result_;
  }

  shutdown_timeout_ns_ = options.trading_config.shutdown_timeout_ns;
  trading_prepared_ = true;
  const auto initialized =
      trading_service_.Init(options.trading_config, options.market_data);
  if (!initialized.ok()) {
    SetErrorIfNotOK(initialized, trading_service_.error_stage(),
                    trading_service_.error_detail());
    Finalize();
    return result_;
  }
  trading_started_ = true;

  const base::Status open_result = library_.Open(options.library_path);
  if (!open_result.ok()) {
    SetErrorIfNotOK(open_result, library_.error_stage(),
                    library_.error_detail());
    Finalize();
    return result_;
  }

  const auto* plugin = library_.plugin();
  const base::ErrorCode create_result = plugin->create(instance_);
  if ((create_result == base::ErrorCode::kOk) != (instance_ != nullptr)) {
    SetErrorIfNotOK(
        {base::ErrorCode::kCorrupt, {}}, "create",
        "strategy factory returned inconsistent status and instance");
  } else if (create_result != base::ErrorCode::kOk) {
    SetErrorIfNotOK({create_result, {}}, "create",
                    "strategy factory returned an error");
  }
  if (!result_.ok()) {
    Finalize();
    return result_;
  }

  base::ConfigError config_error;
  strategy::StrategyConfig& config = instance_->config();
  const base::ErrorCode initialize_result =
      config.Initialize(config_loader.strategy_id(),
                        config_loader.parameters_json(), config_error);
  if (initialize_result != base::ErrorCode::kOk) {
    // Prefix the plugin's parameter-relative path and copy its diagnostic
    // before destroying plugin-owned storage or unloading the library.
    char detail[kErrorDetailSize];
    const char* separator = config_error.field_path[0] == '\0' ? "" : ".";
    std::snprintf(detail, sizeof(detail), "parameters%s%s: %s%s", separator,
                  config_error.field_path,
                  config_error.message[0] != '\0'
                      ? config_error.message
                      : "strategy configuration initialization failed",
                  config_error.truncated ? " [truncated]" : "");
    SetErrorIfNotOK({initialize_result, {}}, "initialize", detail);
    config_loader.Reset();
    Finalize();
    return result_;
  }
  // End the JSON borrow before OnStart so callbacks can rely only on the
  // plugin's owned typed fields, independent of parser/source storage.
  config_loader.Reset();

  BeginCallback(strategy::CallbackPhase::kStart);
  const base::ErrorCode start_result = instance_->OnStart(context_);
  EndCallback();
  if (start_result != base::ErrorCode::kOk) {
    SetErrorIfNotOK({start_result, {}}, "start",
                    "strategy OnStart returned an error");
    Finalize();
    return result_;
  }
  started_ = true;
  const auto status =
      source_.Start(market_data_.subscriptions(), clock_.Now());
  if (!status.ok()) {
    SetErrorIfNotOK(status, "market_data_start", source_.error_detail());
    Finalize();
    return result_;
  }
  source_started_ = true;
  state_ = RunnerState::kRunning;
  timer_.SetRunning(true);
  return result_;
}

namespace {

// Returns whether an optional absolute deadline is due in host-monotonic time.
bool IsDue(std::optional<std::uint64_t> deadline, std::uint64_t now) noexcept {
  return deadline && *deadline <= now;
}

// Lowers earliest to a present candidate, or initializes it if absent. An
// absent candidate leaves the accumulated deadline unchanged.
void IncludeDeadline(std::optional<std::uint64_t>& earliest,
                     std::optional<std::uint64_t> candidate) noexcept {
  if (candidate && (!earliest || *candidate < *earliest)) {
    earliest = candidate;
  }
}

}  // namespace

std::optional<std::uint64_t> StrategyRunner::next_deadline() const noexcept {
  if (state_ != RunnerState::kRunning && state_ != RunnerState::kStopping) {
    return std::nullopt;
  }
  std::optional<std::uint64_t> deadline;
  if (state_ == RunnerState::kRunning && source_started_) {
    IncludeDeadline(deadline, source_.next_deadline());
    IncludeDeadline(deadline, source_.next_service_deadline());
  }
  if (state_ == RunnerState::kRunning) {
    IncludeDeadline(deadline, timer_.deadline());
  }
  if (trading_started_) {
    IncludeDeadline(deadline, trading_service_.next_deadline());
  }
  IncludeDeadline(deadline, shutdown_deadline_);
  return deadline;
}

void StrategyRunner::BeginCallback(strategy::CallbackPhase phase) noexcept {
  callback_active_ = true;
  market_data_.BeginCallback(phase);
  timer_.BeginCallback(phase);
  switch (phase) {
    case strategy::CallbackPhase::kNone:
    case strategy::CallbackPhase::kStart:
    case strategy::CallbackPhase::kStop:
      trading_service_.SetAcceptingRequests(false);
      break;
    case strategy::CallbackPhase::kTick:
    case strategy::CallbackPhase::kOrderUpdate:
    case strategy::CallbackPhase::kFill:
    case strategy::CallbackPhase::kTimer:
      trading_service_.SetAcceptingRequests(state_ == RunnerState::kRunning);
      break;
  }
}

void StrategyRunner::EndCallback() noexcept {
  timer_.EndCallback();
  trading_service_.SetAcceptingRequests(false);
  market_data_.EndCallback();
  callback_active_ = false;
}

void StrategyRunner::SetOrderError(base::Status status, const char* stage,
                                   const char* detail, model::ClientOrderId id,
                                   std::uint64_t sequence) noexcept {
  if (status.ok() || !result_.ok()) {
    return;
  }
  result_ = status;
  error_stage_ = stage;
  std::snprintf(error_detail_, sizeof(error_detail_),
                "client_order_id=%llu sequence=%llu: %s",
                static_cast<unsigned long long>(id.value),
                static_cast<unsigned long long>(sequence), detail);
}

void StrategyRunner::EnterStopping() noexcept {
  if (state_ == RunnerState::kStopping || state_ == RunnerState::kFinished) {
    return;
  }
  state_ = RunnerState::kStopping;
  trading_service_.BeginStopping();
  timer_.BeginStopping();
  const auto now = clock_.Now();
  if (shutdown_timeout_ns_ > std::numeric_limits<std::uint64_t>::max() - now) {
    SetErrorIfNotOK({base::ErrorCode::kResourceExhausted, {}}, "trading_drain",
                    "shutdown deadline overflowed");
    drain_viable_ = false;
  } else {
    shutdown_deadline_ = now + shutdown_timeout_ns_;
  }
  source_started_ = false;
  if (source_prepared_) {
    source_prepared_ = false;
    const auto status = source_.Stop();
    SetErrorIfNotOK(status, "market_data_stop", source_.error_detail());
  }
  if (!result_.ok()) {
    callbacks_enabled_ = false;
  }
}

void StrategyRunner::StopAfterError(bool can_drain) noexcept {
  callbacks_enabled_ = false;
  if (!can_drain) {
    drain_viable_ = false;
  }
  EnterStopping();
}

bool StrategyRunner::IsDrained() const noexcept {
  return trading_service_.is_drained();
}

bool StrategyRunner::FinishStoppingIfReady() noexcept {
  if (state_ != RunnerState::kStopping) {
    return false;
  }
  if (IsDrained()) {
    Finalize();
    return true;
  }
  if (!drain_viable_) {
    Finalize();
    return true;
  }
  if (IsDue(shutdown_deadline_, clock_.Now())) {
    SetErrorIfNotOK(
        {base::ErrorCode::kIoError, {}}, "trading_drain_timeout",
        "outstanding orders or requests did not drain before the deadline");
    callbacks_enabled_ = false;
    Finalize();
    return true;
  }
  return false;
}

void StrategyRunner::CheckStop() noexcept {
  bool stop = false;
  const auto status = environment_.CheckStop(stop);
  if (!status.ok()) {
    SetErrorIfNotOK(status, "stop_check", "could not inspect stop requests");
    StopAfterError(false);
  } else if (stop) {
    EnterStopping();
  }
}

bool StrategyRunner::DispatchQuote(const model::QuoteTick& tick) noexcept {
  bool accepted = false;
  const auto applied = market_data_.ApplyQuote(tick, accepted);
  if (applied != base::ErrorCode::kOk) {
    SetQuoteError({applied, {}}, "quote",
                  "invalid quote from market-data source", tick);
    StopAfterError(true);
    return false;
  }
  if (!accepted) {
    if (ignored_quote_count_ != std::numeric_limits<std::uint64_t>::max()) {
      ++ignored_quote_count_;
    }
    return true;
  }
  const auto observed = trading_service_.OnQuote(tick);
  if (!observed.ok()) {
    SetQuoteError(observed, "trading_quote", trading_service_.error_detail(),
                  tick);
    StopAfterError(false);
    return false;
  }
  BeginCallback(strategy::CallbackPhase::kTick);
  const auto result = instance_->OnTick(context_, tick);
  EndCallback();
  if (result != base::ErrorCode::kOk) {
    SetQuoteError({result, {}}, "tick", "strategy OnTick returned an error",
                  tick);
    StopAfterError(true);
    return false;
  }
  return true;
}

bool StrategyRunner::DispatchTrading(
    const trading::TradingDispatch& dispatch) noexcept {
  if (!callbacks_enabled_) {
    return true;
  }
  if (dispatch.notify_order) {
    BeginCallback(strategy::CallbackPhase::kOrderUpdate);
    const auto code = instance_->OnOrderUpdate(context_, dispatch.update);
    EndCallback();
    if (code != base::ErrorCode::kOk) {
      SetOrderError({code, {}}, "order_update",
                    "strategy OnOrderUpdate returned an error",
                    dispatch.update.order.client_order_id,
                    dispatch.update.order.report_sequence);
      StopAfterError(true);
      return false;
    }
  }
  if (dispatch.has_fill) {
    BeginCallback(strategy::CallbackPhase::kFill);
    const auto code = instance_->OnFill(context_, dispatch.fill);
    EndCallback();
    if (code != base::ErrorCode::kOk) {
      SetOrderError({code, {}}, "fill", "strategy OnFill returned an error",
                    dispatch.fill.client_order_id,
                    dispatch.update.order.report_sequence);
      StopAfterError(true);
      return false;
    }
  }
  return true;
}

StrategyRunner::QuotePollResult StrategyRunner::PollQuote() noexcept {
  const auto now = clock_.Now();
  model::QuoteTick tick;
  const auto polled = source_.Poll(now, tick);
  if (!polled.status.ok() || polled.state == MarketDataPollState::kError) {
    const auto status = polled.status.ok()
                            ? base::Status{base::ErrorCode::kCorrupt, {}}
                            : polled.status;
    SetErrorIfNotOK(status, "market_data_poll", source_.error_detail());
    StopAfterError(true);
    return QuotePollResult::kError;
  }
  if (polled.state == MarketDataPollState::kIdle) {
    if (!source_.is_poll_driven() && IsDue(source_.next_deadline(), now)) {
      SetErrorIfNotOK({base::ErrorCode::kCorrupt, {}}, "market_data_poll",
                      "idle source retained an already-due deadline");
      StopAfterError(true);
      return QuotePollResult::kError;
    }
    if (source_.finished()) {
      EnterStopping();
      return QuotePollResult::kIdle;
    }
    // Return the yield to ProcessBatch so it skips this source until the next
    // batch. Retrying here would multiply a transport's bounded scan allowance.
    return QuotePollResult::kIdle;
  }
  if (polled.state != MarketDataPollState::kEvent) {
    SetErrorIfNotOK({base::ErrorCode::kCorrupt, {}}, "market_data_poll",
                    "source returned an unknown polling state");
    StopAfterError(true);
    return QuotePollResult::kError;
  }
  return DispatchQuote(tick) ? QuotePollResult::kEvent
                             : QuotePollResult::kError;
}

bool StrategyRunner::DispatchTimer() noexcept {
  strategy::TimerEvent event;
  if (!timer_.TryTakeDue(clock_.Now(), event)) {
    return true;
  }
  BeginCallback(strategy::CallbackPhase::kTimer);
  const auto code = instance_->OnTimer(context_, event);
  EndCallback();
  if (code != base::ErrorCode::kOk) {
    SetErrorIfNotOK({code, {}}, "timer", "strategy OnTimer returned an error");
    StopAfterError(true);
    return false;
  }
  return true;
}

bool StrategyRunner::PollTrading() noexcept {
  const auto now = clock_.Now();
  trading::TradingDispatch dispatch;
  bool event = false;
  const auto polled = trading_service_.Poll(now, dispatch, event);
  if (!polled.ok()) {
    SetErrorIfNotOK(polled, trading_service_.error_stage(),
                    trading_service_.error_detail());
    StopAfterError(false);
    return false;
  }
  if (!event) return true;
  return DispatchTrading(dispatch);
}

void StrategyRunner::ProcessBatch() noexcept {
  bool market_idle = false;
  for (std::size_t count = 0; count < kMaxStrategyBatchSize; ++count) {
    const auto now = clock_.Now();
    const bool ready[] = {
        !market_idle && state_ == RunnerState::kRunning && source_started_ &&
            IsDue(source_.next_deadline(), now),
        trading_started_ && trading_service_.has_ready_event(now),
        state_ == RunnerState::kRunning && callbacks_enabled_ &&
            IsDue(timer_.deadline(), now)};
    constexpr std::size_t kEventSourceCount = sizeof(ready) / sizeof(ready[0]);
    std::size_t selected = kEventSourceCount;
    for (std::size_t offset = 0; offset < kEventSourceCount; ++offset) {
      const auto candidate = (next_event_source_ + offset) % kEventSourceCount;
      if (ready[candidate]) {
        selected = candidate;
        break;
      }
    }
    if (selected == kEventSourceCount) {
      return;
    }
    next_event_source_ =
        static_cast<std::uint8_t>((selected + 1) % kEventSourceCount);
    if (selected == 0) {
      const auto polled = PollQuote();
      if (polled == QuotePollResult::kError) {
        return;
      }
      market_idle = polled == QuotePollResult::kIdle;
    } else if (selected == 1) {
      if (!PollTrading()) {
        return;
      }
    } else if (!DispatchTimer()) {
      return;
    }
  }
}

base::Status StrategyRunner::Run() noexcept {
  if (state_ != RunnerState::kRunning || callback_active_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  ResetError();
  for (;;) {
    CheckStop();
    if (FinishStoppingIfReady()) {
      return result_;
    }
    if (state_ == RunnerState::kRunning && source_started_) {
      // Keep discovery/health maintenance outside event batches. Source failure
      // stops quote delivery while a usable trading channel continues draining.
      const auto serviced = source_.Service(clock_.Now());
      if (!serviced.ok()) {
        SetErrorIfNotOK(serviced, "market_data_service",
                        source_.error_detail());
        StopAfterError(true);
      }
    }
    if (state_ == RunnerState::kRunning && source_started_ &&
        source_.finished()) {
      EnterStopping();
    }
    if (trading_started_ && drain_viable_) {
      const auto sent = trading_service_.ProcessRequests(clock_.Now());
      if (!sent.ok()) {
        SetErrorIfNotOK(sent, "trading_send", trading_service_.error_detail());
        StopAfterError(false);
      }
    }
    if (FinishStoppingIfReady()) {
      return result_;
    }
    ProcessBatch();
    CheckStop();
    if (FinishStoppingIfReady()) {
      return result_;
    }
    const auto now = clock_.Now();
    const auto deadline = next_deadline();
    if (IsDue(deadline, now)) {
      continue;
    }
    const auto timeout =
        deadline ? std::optional<std::uint64_t>{*deadline - now} : std::nullopt;
    const auto waited = environment_.Wait(timeout);
    if (!waited.ok()) {
      SetErrorIfNotOK(waited, "wait", "could not wait for the next event");
      StopAfterError(false);
    }
  }
}

void StrategyRunner::ReportUnconfirmed() const noexcept {
  bool reported_cash = false;
  for (std::size_t index = 0; index < trading_service_.order_count(); ++index) {
    model::OrderSnapshot order;
    if (!trading_service_.TryGetOrder({index + 1}, order)) {
      continue;
    }
    const bool terminal = order.state == model::OrderState::kFilled ||
                          order.state == model::OrderState::kCanceled ||
                          order.state == model::OrderState::kRejected;
    if (terminal && !order.cancel_pending) {
      continue;
    }
    if (!reported_cash) {
      model::CashSnapshot cash;
      if (trading_service_.TryGetCash(cash)) {
        std::fprintf(stderr,
                     "yquant-strategyd: cash_balance=%lld reserved=%lld "
                     "available=%lld unit=micro_yuan\n",
                     static_cast<long long>(cash.balance),
                     static_cast<long long>(cash.reserved),
                     static_cast<long long>(cash.available));
      }
      model::FuturesAccountSnapshot futures;
      if (trading_service_.TryGetFuturesAccount(futures)) {
        std::fprintf(
            stderr,
            "yquant-strategyd: futures_balance=%lld equity=%lld available=%lld "
            "margin=%lld reserved=%lld realized_pnl=%lld unrealized_pnl=%lld "
            "commission=%lld unit=micro_yuan\n",
            static_cast<long long>(futures.balance),
            static_cast<long long>(futures.equity),
            static_cast<long long>(futures.available),
            static_cast<long long>(futures.margin),
            static_cast<long long>(futures.reserved),
            static_cast<long long>(futures.realized_pnl),
            static_cast<long long>(futures.unrealized_pnl),
            static_cast<long long>(futures.commission));
      }
      reported_cash = true;
    }
    std::fprintf(stderr,
                 "yquant-strategyd: unconfirmed_order=%llu state=%u "
                 "instrument_id=%llu side=%u price=%lld quantity=%lld "
                 "filled=%lld leaves=%lld cancel_pending=%u "
                 "cancel_request=%llu effect=%u\n",
                 static_cast<unsigned long long>(order.client_order_id.value),
                 static_cast<unsigned>(order.state),
                 static_cast<unsigned long long>(order.instrument_id.value),
                 static_cast<unsigned>(order.side),
                 static_cast<long long>(order.limit_price),
                 static_cast<long long>(order.original_qty),
                 static_cast<long long>(order.cumulative_filled_qty),
                 static_cast<long long>(order.leaves_qty),
                 static_cast<unsigned>(order.cancel_pending),
                 static_cast<unsigned long long>(order.cancel_request_id.value),
                 static_cast<unsigned>(order.position_effect));
    model::PositionSnapshot position;
    if (trading_service_.TryGetPosition(order.instrument_id, position)) {
      std::fprintf(stderr,
                   "yquant-strategyd: instrument_id=%llu total_qty=%lld "
                   "available_qty=%lld reserved_qty=%lld\n",
                   static_cast<unsigned long long>(order.instrument_id.value),
                   static_cast<long long>(position.total_qty),
                   static_cast<long long>(position.available_qty),
                   static_cast<long long>(position.reserved_qty));
    }
    model::FuturesPositionSnapshot futures;
    if (trading_service_.TryGetFuturesPosition(order.instrument_id, futures)) {
      std::fprintf(stderr,
                   "yquant-strategyd: instrument_id=%llu long_today=%lld/%lld "
                   "long_yesterday=%lld/%lld "
                   "short_today=%lld/%lld short_yesterday=%lld/%lld "
                   "unit=quantity/frozen_lots\n",
                   static_cast<unsigned long long>(order.instrument_id.value),
                   static_cast<long long>(futures.long_today.quantity),
                   static_cast<long long>(futures.long_today.frozen_qty),
                   static_cast<long long>(futures.long_yesterday.quantity),
                   static_cast<long long>(futures.long_yesterday.frozen_qty),
                   static_cast<long long>(futures.short_today.quantity),
                   static_cast<long long>(futures.short_today.frozen_qty),
                   static_cast<long long>(futures.short_yesterday.quantity),
                   static_cast<long long>(futures.short_yesterday.frozen_qty));
    }
  }
}

void StrategyRunner::Finalize() noexcept {
  if (state_ == RunnerState::kFinished) {
    return;
  }
  state_ = RunnerState::kStopping;
  trading_service_.SetAcceptingRequests(false);
  timer_.BeginStopping();
  source_started_ = false;
  if (source_prepared_) {
    source_prepared_ = false;
    const auto status = source_.Stop();
    SetErrorIfNotOK(status, "market_data_stop", source_.error_detail());
  }
  // Keep callback services alive through OnStop and plugin destruction; tear
  // down transport only after unconfirmed facts have been reported.
  if (started_) {
    started_ = false;
    BeginCallback(strategy::CallbackPhase::kStop);
    const auto stopped = instance_->OnStop(context_);
    EndCallback();
    SetErrorIfNotOK({stopped, {}}, "stop", "strategy OnStop returned an error");
  }
  if (instance_ != nullptr) {
    library_.plugin()->destroy(instance_);
    instance_ = nullptr;
  }
  const auto closed = library_.Close();
  SetErrorIfNotOK(closed, library_.error_stage(), library_.error_detail());
  if (!IsDrained()) {
    ReportUnconfirmed();
  }
  trading_started_ = false;
  if (trading_prepared_) {
    trading_prepared_ = false;
    const auto stopped = trading_service_.Stop();
    SetErrorIfNotOK(stopped, "trading_stop", trading_service_.error_detail());
  }
  trading_service_.Reset();
  timer_.Reset();
  market_data_.Reset();
  state_ = RunnerState::kFinished;
}

}  // namespace yquant::strategyd
