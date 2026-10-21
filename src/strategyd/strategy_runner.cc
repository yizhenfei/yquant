#include "strategyd/strategy_runner.h"

#include <cstdio>
#include <limits>

#include "strategyd/strategy_config_loader.h"
#include "strategyd/trading_config.h"

namespace yquant::strategyd {

StrategyRunner::StrategyRunner(const MonotonicClock* clock,
                               MarketDataSource* source,
                               EventLoopEnvironment* environment,
                               TradingAdapter* trading_adapter) noexcept
    : clock_(clock != nullptr ? *clock : default_clock_),
      market_data_(clock_),
      trading_(clock_, market_data_),
      remote_trading_(clock_, market_data_),
      context_(clock_, market_data_, trading_),
      source_(source),
      trading_adapter_(trading_adapter != nullptr ? *trading_adapter
                                                  : default_trading_adapter_),
      startup_environment_(environment) {}

StrategyRunner::~StrategyRunner() noexcept {
  // Explicit Stop reports errors; this is the ownership safety net.
  Finalize();
}

void StrategyRunner::ResetError() noexcept {
  result_ = {};
  error_stage_ = "";
  error_detail_[0] = '\0';
}

void StrategyRunner::SetError(base::Status status, const char* stage,
                              const char* detail) noexcept {
  if (status.ok() || !result_.ok()) {
    return;
  }
  result_ = status;
  error_stage_ = stage;
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
}

void StrategyRunner::SetConfigError(base::Status status, const char* stage,
                                    const base::ConfigError& error) noexcept {
  char detail[kErrorDetailSize];
  std::snprintf(detail, sizeof(detail), "%s: %s%s", error.field_path,
                error.message, error.truncated ? " [truncated]" : "");
  SetError(status, stage, detail);
}

void StrategyRunner::SetQuoteError(base::Status status, const char* stage,
                                   const char* detail,
                                   const model::QuoteTick& tick) noexcept {
  char copied[kErrorDetailSize];
  std::snprintf(copied, sizeof(copied),
                "source_id=%u instrument_id=%llu sequence=%llu: %s",
                tick.source_id.value,
                static_cast<unsigned long long>(tick.instrument_id.value),
                static_cast<unsigned long long>(tick.sequence), detail);
  SetError(status, stage, copied);
}

bool StrategyRunner::CancelStartupIfRequested() noexcept {
  if (startup_environment_ == nullptr) {
    return false;
  }
  bool stop = false;
  const auto status = startup_environment_->CheckStop(stop);
  if (!status.ok()) {
    SetError(status, "signal", "could not inspect termination signals");
  }
  if (stop || !result_.ok()) {
    Finalize();
    return true;
  }
  return false;
}

base::Status StrategyRunner::Handle(LoadAndStart command) noexcept {
  ResetError();
  if (state_ != RunnerState::kIdle) {
    SetError({base::ErrorCode::kInvalidState, {}}, "command",
             "a strategy can only be loaded in the idle state");
    return result_;
  }

  state_ = RunnerState::kStarting;
  if (CancelStartupIfRequested()) {
    return result_;
  }
  StrategyConfigLoader config_loader;
  const base::Status config_result = config_loader.Load(command.config_path);
  if (!config_result.ok()) {
    SetConfigError(config_result, "config", config_loader.error());
    Finalize();
    return result_;
  }
  strategy_id_ = config_loader.strategy_id();
  if (CancelStartupIfRequested()) {
    return result_;
  }

  MarketDataConfig dictionary;
  if (command.market_data_config_path != nullptr) {
    MarketDataConfigLoader market_loader;
    const auto status = market_loader.Load(command.market_data_config_path);
    if (!status.ok()) {
      SetConfigError(status, "market_data_config", market_loader.error());
      Finalize();
      return result_;
    }
    dictionary = market_loader.config();
    if (source_ == nullptr) {
      source_ = market_loader.config().adapter == MarketDataAdapter::kShm
                    ? static_cast<MarketDataSource*>(&shm_source_)
                    : static_cast<MarketDataSource*>(&mock_source_);
    }
    // Prepare may acquire resources before reporting failure; Stop is safe
    // even when preparation did not finish.
    source_prepared_ = true;
    const auto prepare_status = source_->Prepare(market_loader.config());
    if (!prepare_status.ok()) {
      SetError(prepare_status, "market_data_prepare", source_->error_detail());
      Finalize();
      return result_;
    }
    const auto market_data_status =
        market_data_.Configure(market_loader.config());
    if (!market_data_status.ok()) {
      SetError(market_data_status, "market_data_prepare",
               "could not prepare the instrument dictionary");
      Finalize();
      return result_;
    }
  }
  if (CancelStartupIfRequested()) {
    return result_;
  }

  if (command.trading_config_path != nullptr) {
    if (command.market_data_config_path == nullptr) {
      SetError({base::ErrorCode::kInvalidArgument, {}}, "trading_config",
               "trading requires an instrument dictionary from market data");
      Finalize();
      return result_;
    }
    TradingConfigLoader trading_loader;
    const auto loaded = trading_loader.Load(command.trading_config_path);
    if (!loaded.ok()) {
      SetConfigError(loaded, "trading_config", trading_loader.error());
      Finalize();
      return result_;
    }
    shutdown_timeout_ns_ = trading_loader.config().shutdown_timeout_ns;
    trading_prepared_ = true;
    remote_trading_selected_ =
        trading_loader.config().adapter == TradingAdapterKind::kTcp;
    if (remote_trading_selected_) {
      trading_runtime_ = &remote_trading_;
      context_.SetTrading(*trading_runtime_);
      const auto initialized =
          remote_trading_.Init(trading_loader.config(), dictionary);
      if (!initialized.ok()) {
        SetError(initialized, "trading_connect",
                 remote_trading_.error_detail());
        Finalize();
        return result_;
      }
      trading_started_ = true;
    } else {
      const auto prepared = trading_adapter_.Prepare(trading_loader.config());
      if (!prepared.ok()) {
        SetError(prepared, "trading_prepare", trading_adapter_.error_detail());
        Finalize();
        return result_;
      }
      const auto started = trading_adapter_.Start(clock_.Now());
      if (!started.ok()) {
        SetError(started, "trading_start", trading_adapter_.error_detail());
        Finalize();
        return result_;
      }
      trading_started_ = true;
      if (CancelStartupIfRequested()) {
        return result_;
      }
      TradingAccountSnapshot account;
      const auto snapshot = trading_adapter_.ReadAccountSnapshot(account);
      if (!snapshot.ok()) {
        SetError(snapshot, "trading_account", trading_adapter_.error_detail());
        Finalize();
        return result_;
      }
      const auto configured = trading_.Configure(account);
      if (!configured.ok()) {
        SetError(configured, "trading_account",
                 trading_runtime_->error_detail());
        Finalize();
        return result_;
      }
    }
  }
  if (CancelStartupIfRequested()) {
    return result_;
  }

  const base::Status open_result = library_.Open(command.library_path);
  if (!open_result.ok()) {
    SetError(open_result, library_.error_stage(), library_.error_detail());
    Finalize();
    return result_;
  }
  if (CancelStartupIfRequested()) {
    return result_;
  }

  const auto* plugin = library_.plugin();
  const base::ErrorCode create_result = plugin->create(instance_);
  if ((create_result == base::ErrorCode::kOk) != (instance_ != nullptr)) {
    SetError({base::ErrorCode::kCorrupt, {}}, "create",
             "strategy factory returned inconsistent status and instance");
  } else if (create_result != base::ErrorCode::kOk) {
    SetError({create_result, {}}, "create",
             "strategy factory returned an error");
  }
  if (!result_.ok()) {
    Finalize();
    return result_;
  }
  if (CancelStartupIfRequested()) {
    return result_;
  }

  base::ConfigError config_error;
  strategy::StrategyConfig& config = instance_->config();
  const base::ErrorCode initialize_result =
      config.Initialize(config_loader.strategy_id(),
                        config_loader.parameters_json(), config_error);
  if (initialize_result != base::ErrorCode::kOk) {
    // Plugins report paths relative to parameters. Copy both parts before
    // destroying the instance or releasing its library.
    char detail[kErrorDetailSize];
    const char* separator = config_error.field_path[0] == '\0' ? "" : ".";
    std::snprintf(detail, sizeof(detail), "parameters%s%s: %s%s", separator,
                  config_error.field_path,
                  config_error.message[0] != '\0'
                      ? config_error.message
                      : "strategy configuration initialization failed",
                  config_error.truncated ? " [truncated]" : "");
    SetError({initialize_result, {}}, "initialize", detail);
    config_loader.Reset();
    Finalize();
    return result_;
  }
  if (!config.ready()) {
    SetError({base::ErrorCode::kCorrupt, {}}, "initialize",
             "strategy configuration did not become ready");
    config_loader.Reset();
    Finalize();
    return result_;
  }
  // JSON views are initialization-only. OnStart must use owned typed fields.
  config_loader.Reset();

  if (CancelStartupIfRequested()) {
    return result_;
  }
  BeginCallback(CallbackPhase::kStart);
  const base::ErrorCode start_result = instance_->OnStart(context_);
  EndCallback();
  if (start_result != base::ErrorCode::kOk) {
    SetError({start_result, {}}, "start", "strategy OnStart returned an error");
    Finalize();
    return result_;
  }
  started_ = true;
  if (CancelStartupIfRequested()) {
    return result_;
  }
  if (source_prepared_) {
    const auto status =
        source_->Start(market_data_.subscriptions(), clock_.Now());
    if (!status.ok()) {
      SetError(status, "market_data_start", source_->error_detail());
      Finalize();
      return result_;
    }
    source_started_ = true;
  }
  if (CancelStartupIfRequested()) {
    return result_;
  }
  state_ = RunnerState::kRunning;
  trading_runtime_->SetRunning(true);
  return result_;
}

base::Status StrategyRunner::Handle(Stop) noexcept {
  if (callback_active_ || state_ == RunnerState::kStarting) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  if (state_ == RunnerState::kStopping) {
    return {};
  }
  ResetError();
  if (state_ == RunnerState::kIdle) {
    Finalize();
  } else if (state_ == RunnerState::kRunning) {
    stop_requested_ = true;
  }
  return result_;
}

namespace {

// Both sources use the host's monotonic time domain.
bool IsDue(std::optional<std::uint64_t> deadline, std::uint64_t now) noexcept {
  return deadline && *deadline <= now;
}

// Keeps the earliest absolute deadline; absent inputs do not schedule work.
void IncludeDeadline(std::optional<std::uint64_t>& earliest,
                     std::optional<std::uint64_t> candidate) noexcept {
  if (candidate && (!earliest || *candidate < *earliest)) {
    earliest = candidate;
  }
}

// Failure diagnostics preserve opaque identifier bytes, including embedded NUL.
// A 64-byte public identifier fits in the required 129-byte output buffer.
void CopyIdHex(std::string_view value, char (&out)[129]) noexcept {
  constexpr char kHex[] = "0123456789abcdef";
  for (std::size_t index = 0; index < value.size(); ++index) {
    const auto byte = static_cast<unsigned char>(value[index]);
    out[2 * index] = kHex[byte >> 4];
    out[2 * index + 1] = kHex[byte & 15];
  }
  out[2 * value.size()] = '\0';
}

// Logs an unapplied source event even if an earlier callback error remains the
// primary exit reason. Called only on the low-frequency failure path.
void ReportUnapplied(const TradingEvent& event) noexcept {
  char broker[129];
  char execution[129];
  CopyIdHex(event.broker_order_id.view(), broker);
  CopyIdHex(event.fill.execution_id.view(), execution);
  std::fprintf(
      stderr,
      "yquant-strategyd: unapplied_trading_event client_order_id=%llu "
      "sequence=%llu kind=%u cumulative=%lld fill_price=%lld fill_qty=%lld "
      "cancel_request=%llu broker_id_hex=%s execution_id_hex=%s\n",
      static_cast<unsigned long long>(event.client_order_id.value),
      static_cast<unsigned long long>(event.report_sequence),
      static_cast<unsigned>(event.kind),
      static_cast<long long>(event.cumulative_filled_qty),
      static_cast<long long>(event.fill.price),
      static_cast<long long>(event.fill.quantity),
      static_cast<unsigned long long>(event.cancel_request_id.value), broker,
      execution);
}

}  // namespace

std::optional<std::uint64_t> StrategyRunner::next_deadline() const noexcept {
  if (state_ != RunnerState::kRunning && state_ != RunnerState::kStopping) {
    return std::nullopt;
  }
  std::optional<std::uint64_t> deadline;
  if (stop_requested_) {
    deadline = 0;
  }
  if (state_ == RunnerState::kRunning && source_started_) {
    IncludeDeadline(deadline, source_->next_deadline());
    IncludeDeadline(deadline, source_->next_service_deadline());
  }
  if (trading_started_) {
    IncludeDeadline(deadline, trading_runtime_->next_deadline());
    if (!remote_trading_selected_)
      IncludeDeadline(deadline, trading_adapter_.next_deadline());
  }
  IncludeDeadline(deadline, shutdown_deadline_);
  return deadline;
}

void StrategyRunner::BeginCallback(CallbackPhase phase) noexcept {
  callback_active_ = true;
  market_data_.BeginCallback(phase);
  trading_runtime_->BeginCallback(phase);
}

void StrategyRunner::EndCallback() noexcept {
  trading_runtime_->EndCallback();
  market_data_.EndCallback();
  callback_active_ = false;
}

void StrategyRunner::SetOrderError(base::Status status, const char* stage,
                                   const char* detail, model::ClientOrderId id,
                                   std::uint64_t sequence) noexcept {
  char copied[kErrorDetailSize];
  std::snprintf(copied, sizeof(copied),
                "client_order_id=%llu sequence=%llu: %s",
                static_cast<unsigned long long>(id.value),
                static_cast<unsigned long long>(sequence), detail);
  SetError(status, stage, copied);
}

void StrategyRunner::EnterStopping() noexcept {
  if (state_ == RunnerState::kStopping || state_ == RunnerState::kFinished) {
    return;
  }
  state_ = RunnerState::kStopping;
  stop_requested_ = false;
  trading_runtime_->BeginStopping();
  const auto now = clock_.Now();
  if (shutdown_timeout_ns_ > std::numeric_limits<std::uint64_t>::max() - now) {
    SetError({base::ErrorCode::kResourceExhausted, {}}, "trading_drain",
             "shutdown deadline overflowed");
    drain_viable_ = false;
  } else {
    shutdown_deadline_ = now + shutdown_timeout_ns_;
  }
  source_started_ = false;
  if (source_prepared_) {
    source_prepared_ = false;
    const auto status = source_->Stop();
    SetError(status, "market_data_stop", source_->error_detail());
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
  return trading_runtime_->is_drained() &&
         (!trading_started_ || remote_trading_selected_ ||
          trading_adapter_.is_drained());
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
    SetError(
        {base::ErrorCode::kIoError, {}}, "trading_drain_timeout",
        "outstanding orders or requests did not drain before the deadline");
    callbacks_enabled_ = false;
    Finalize();
    return true;
  }
  return false;
}

void StrategyRunner::CheckStop(EventLoopEnvironment& environment) noexcept {
  bool stop = false;
  const auto status = environment.CheckStop(stop);
  if (!status.ok()) {
    SetError(status, "signal", "could not inspect termination signals");
    StopAfterError(false);
  } else if (stop || stop_requested_) {
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
  BeginCallback(CallbackPhase::kTick);
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

bool StrategyRunner::DispatchTrading(const TradingDispatch& dispatch) noexcept {
  if (!callbacks_enabled_) {
    return true;
  }
  if (dispatch.notify_order) {
    BeginCallback(CallbackPhase::kOrderUpdate);
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
    BeginCallback(CallbackPhase::kFill);
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
  const auto polled = source_->Poll(now, tick);
  if (!polled.status.ok() || polled.state == MarketDataPollState::kError) {
    const auto status = polled.status.ok()
                            ? base::Status{base::ErrorCode::kCorrupt, {}}
                            : polled.status;
    SetError(status, "market_data_poll", source_->error_detail());
    StopAfterError(true);
    return QuotePollResult::kError;
  }
  if (polled.state == MarketDataPollState::kIdle) {
    if (!source_->is_poll_driven() && IsDue(source_->next_deadline(), now)) {
      SetError({base::ErrorCode::kCorrupt, {}}, "market_data_poll",
               "idle source retained an already-due deadline");
      StopAfterError(true);
      return QuotePollResult::kError;
    }
    // SHM already bounded its scan internally. Retry only in the next Run
    // iteration, so a transport yield does not multiply its scan budget here.
    return QuotePollResult::kIdle;
  }
  if (polled.state != MarketDataPollState::kEvent) {
    SetError({base::ErrorCode::kCorrupt, {}}, "market_data_poll",
             "source returned an unknown polling state");
    StopAfterError(true);
    return QuotePollResult::kError;
  }
  return DispatchQuote(tick) ? QuotePollResult::kEvent
                             : QuotePollResult::kError;
}

bool StrategyRunner::PollTrading() noexcept {
  const auto now = clock_.Now();
  const bool local = trading_runtime_->has_local_updates();
  const bool remote =
      IsDue(remote_trading_selected_ ? trading_runtime_->next_deadline()
                                     : trading_adapter_.next_deadline(),
            now);
  TradingDispatch dispatch;
  if (local && (!remote || prefer_local_)) {
    prefer_local_ = false;
    const auto code = trading_runtime_->ApplyLocalUpdate(dispatch);
    if (code != base::ErrorCode::kOk) {
      SetError({code, {}}, "trading_local_update",
               trading_runtime_->error_detail());
      StopAfterError(false);
      return false;
    }
  } else if (remote_trading_selected_) {
    prefer_local_ = true;
    bool event = false;
    const auto polled = remote_trading_.Poll(now, dispatch, event);
    if (!polled.ok()) {
      SetError(polled, "trading_poll", remote_trading_.error_detail());
      StopAfterError(false);
      return false;
    }
    if (!event) return true;
  } else {
    prefer_local_ = true;
    TradingEvent event;
    const auto polled = trading_adapter_.Poll(now, event);
    if (!polled.status.ok() || polled.state == TradingPollState::kError) {
      const auto status = polled.status.ok()
                              ? base::Status{base::ErrorCode::kCorrupt, {}}
                              : polled.status;
      SetError(status, "trading_poll", trading_adapter_.error_detail());
      StopAfterError(false);
      return false;
    }
    if (polled.state == TradingPollState::kIdle) {
      if (IsDue(trading_adapter_.next_deadline(), now)) {
        SetError({base::ErrorCode::kCorrupt, {}}, "trading_poll",
                 "idle trading adapter retained an already-due deadline");
        StopAfterError(false);
        return false;
      }
      return true;
    }
    if (polled.state != TradingPollState::kEvent) {
      SetError({base::ErrorCode::kCorrupt, {}}, "trading_poll",
               "adapter returned an unknown polling state");
      StopAfterError(false);
      return false;
    }
    const auto applied = trading_runtime_->ApplyEvent(event, dispatch);
    if (applied != base::ErrorCode::kOk) {
      ReportUnapplied(event);
      SetOrderError({applied, {}}, "trading_event",
                    trading_runtime_->error_detail(), event.client_order_id,
                    event.report_sequence);
      StopAfterError(false);
      return false;
    }
  }
  return DispatchTrading(dispatch);
}

void StrategyRunner::ProcessBatch() noexcept {
  bool market_idle = false;
  for (std::size_t count = 0; count < kMaxStrategyBatchSize; ++count) {
    const auto now = clock_.Now();
    const bool market = !market_idle && state_ == RunnerState::kRunning &&
                        source_started_ && IsDue(source_->next_deadline(), now);
    const bool trading =
        trading_started_ &&
        (trading_runtime_->has_local_updates() ||
         IsDue(remote_trading_selected_ ? trading_runtime_->next_deadline()
                                        : trading_adapter_.next_deadline(),
               now));
    if (!market && !trading) {
      return;
    }
    if (market && (!trading || prefer_market_)) {
      prefer_market_ = false;
      const auto polled = PollQuote();
      if (polled == QuotePollResult::kError) {
        return;
      }
      market_idle = polled == QuotePollResult::kIdle;
    } else {
      prefer_market_ = true;
      if (!PollTrading()) {
        return;
      }
    }
  }
}

base::Status StrategyRunner::Run(EventLoopEnvironment& environment) noexcept {
  if (state_ != RunnerState::kRunning || callback_active_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  ResetError();
  for (;;) {
    CheckStop(environment);
    if (FinishStoppingIfReady()) {
      return result_;
    }
    if (state_ == RunnerState::kRunning && source_started_) {
      // Discovery/heartbeat work is deliberately outside the hot polling
      // batch. A failed market source must not prevent broker-order draining.
      const auto serviced = source_->Service(clock_.Now());
      if (!serviced.ok()) {
        SetError(serviced, "market_data_service", source_->error_detail());
        StopAfterError(true);
      }
    }
    if (state_ == RunnerState::kStopping && trading_started_) {
      const auto scheduled = trading_runtime_->ScheduleStopping(clock_.Now());
      if (!scheduled.ok()) {
        SetError(scheduled, "trading_drain", trading_runtime_->error_detail());
        StopAfterError(false);
      }
    }
    if (trading_started_ && drain_viable_) {
      const auto sent =
          trading_runtime_->ProcessRequests(trading_adapter_, clock_.Now());
      if (!sent.ok()) {
        SetError(sent, "trading_send", trading_runtime_->error_detail());
        StopAfterError(false);
      }
    }
    if (FinishStoppingIfReady()) {
      return result_;
    }
    ProcessBatch();
    CheckStop(environment);
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
    const auto waited = environment.Wait(timeout);
    if (!waited.ok()) {
      SetError(waited, "wait", "could not wait for data or termination");
      StopAfterError(false);
    }
  }
}

void StrategyRunner::ReportUnconfirmed() const noexcept {
  bool reported_cash = false;
  for (std::size_t index = 0; index < trading_runtime_->order_count();
       ++index) {
    model::OrderSnapshot order;
    if (!trading_runtime_->TryGetOrder({index + 1}, order)) {
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
      if (trading_runtime_->TryGetCash(cash)) {
        std::fprintf(stderr,
                     "yquant-strategyd: cash_balance=%lld reserved=%lld "
                     "available=%lld unit=micro_yuan\n",
                     static_cast<long long>(cash.balance),
                     static_cast<long long>(cash.reserved),
                     static_cast<long long>(cash.available));
      }
      reported_cash = true;
    }
    std::fprintf(
        stderr,
        "yquant-strategyd: unconfirmed_order=%llu state=%u "
        "instrument_id=%llu side=%u price=%lld quantity=%lld "
        "filled=%lld leaves=%lld cancel_pending=%u cancel_request=%llu\n",
        static_cast<unsigned long long>(order.client_order_id.value),
        static_cast<unsigned>(order.state),
        static_cast<unsigned long long>(order.instrument_id.value),
        static_cast<unsigned>(order.side),
        static_cast<long long>(order.limit_price),
        static_cast<long long>(order.original_qty),
        static_cast<long long>(order.cumulative_filled_qty),
        static_cast<long long>(order.leaves_qty),
        static_cast<unsigned>(order.cancel_pending),
        static_cast<unsigned long long>(order.cancel_request_id.value));
    model::PositionSnapshot position;
    if (trading_runtime_->TryGetPosition(order.instrument_id, position)) {
      std::fprintf(stderr,
                   "yquant-strategyd: instrument_id=%llu total_qty=%lld "
                   "available_qty=%lld reserved_qty=%lld\n",
                   static_cast<unsigned long long>(order.instrument_id.value),
                   static_cast<long long>(position.total_qty),
                   static_cast<long long>(position.available_qty),
                   static_cast<long long>(position.reserved_qty));
    }
  }
}

void StrategyRunner::Finalize() noexcept {
  if (state_ == RunnerState::kFinished) {
    return;
  }
  state_ = RunnerState::kStopping;
  trading_runtime_->SetRunning(false);
  source_started_ = false;
  if (source_prepared_) {
    source_prepared_ = false;
    const auto status = source_->Stop();
    SetError(status, "market_data_stop", source_->error_detail());
  }
  if (started_) {
    started_ = false;
    BeginCallback(CallbackPhase::kStop);
    const auto stopped = instance_->OnStop(context_);
    EndCallback();
    SetError({stopped, {}}, "stop", "strategy OnStop returned an error");
  }
  if (instance_ != nullptr) {
    library_.plugin()->destroy(instance_);
    instance_ = nullptr;
  }
  const auto closed = library_.Close();
  SetError(closed, library_.error_stage(), library_.error_detail());
  if (!IsDrained()) {
    ReportUnconfirmed();
  }
  trading_started_ = false;
  if (trading_prepared_) {
    trading_prepared_ = false;
    if (!remote_trading_selected_) {
      const auto stopped = trading_adapter_.Stop();
      SetError(stopped, "trading_stop", trading_adapter_.error_detail());
    }
  }
  trading_runtime_->Reset();
  market_data_.Reset();
  state_ = RunnerState::kFinished;
}

}  // namespace yquant::strategyd
