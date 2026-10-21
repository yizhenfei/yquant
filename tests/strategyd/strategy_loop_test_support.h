#ifndef YQUANT_TESTS_STRATEGYD_STRATEGY_LOOP_TEST_SUPPORT_H_
#define YQUANT_TESTS_STRATEGYD_STRATEGY_LOOP_TEST_SUPPORT_H_

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "runner_test_harness.h"
#include "strategyd/market_data_source.h"
#include "yquant/base/event_loop_environment.h"
#include "yquant/base/monotonic_clock.h"
#include "yquant/trading/trading_adapter.h"

namespace yquant::strategyd::loop_testing {

inline constexpr std::uint64_t kMs = 1000000;

class Clock final : public base::MonotonicClock {
 public:
  std::uint64_t Now() const noexcept { return now; }
  std::uint64_t now = 0;
};

// Records DLL callback markers without changing the production interfaces.
class EventPipe {
 public:
  EventPipe(const EventPipe&) = delete;
  EventPipe& operator=(const EventPipe&) = delete;
  EventPipe() {
    const char* previous = std::getenv("YQUANT_TEST_EVENT_FD");
    had_previous_ = previous != nullptr;
    if (had_previous_) previous_ = previous;
    if (pipe(fds_) != 0 || fcntl(fds_[0], F_SETFL, O_NONBLOCK) == -1) return;
    const auto fd = std::to_string(fds_[1]);
    valid_ = setenv("YQUANT_TEST_EVENT_FD", fd.c_str(), 1) == 0;
  }
  ~EventPipe() {
    if (had_previous_)
      setenv("YQUANT_TEST_EVENT_FD", previous_.c_str(), 1);
    else
      unsetenv("YQUANT_TEST_EVENT_FD");
    for (int fd : fds_)
      if (fd >= 0) close(fd);
  }
  bool valid() const { return valid_; }
  std::string Read() {
    std::string text;
    char bytes[512];
    for (;;) {
      const auto count = read(fds_[0], bytes, sizeof(bytes));
      if (count > 0)
        text.append(bytes, static_cast<std::size_t>(count));
      else if (count == -1 && errno == EINTR)
        continue;
      else
        return text;
    }
  }

 private:
  int fds_[2] = {-1, -1};
  bool valid_ = false;
  bool had_previous_ = false;
  std::string previous_;
};

// Captures low-frequency strategy summaries in a regular file, so callbacks
// cannot block on a full pipe. Setup/reading are outside the event hot path.
class StderrCapture {
 public:
  StderrCapture(const StderrCapture&) = delete;
  StderrCapture& operator=(const StderrCapture&) = delete;
  StderrCapture() {
    file_ = std::tmpfile();
    if (file_ == nullptr) return;
    std::fflush(stdout);
    saved_ = dup(STDOUT_FILENO);
    valid_ = saved_ >= 0 && dup2(fileno(file_), STDOUT_FILENO) >= 0;
  }
  ~StderrCapture() {
    Restore();
    if (file_ != nullptr) std::fclose(file_);
  }
  bool valid() const { return valid_; }
  std::string Read() {
    Restore();
    std::rewind(file_);
    std::string text;
    char bytes[1024];
    while (const auto count = std::fread(bytes, 1, sizeof(bytes), file_))
      text.append(bytes, count);
    return text;
  }

 private:
  void Restore() {
    if (saved_ >= 0) {
      std::fflush(stdout);
      (void)dup2(saved_, STDOUT_FILENO);
      close(saved_);
      saved_ = -1;
    }
  }
  std::FILE* file_ = nullptr;
  int saved_ = -1;
  bool valid_ = false;
};

struct QuoteFrame {
  std::uint64_t at = 0;
  std::int64_t bid = 1000;
  std::int64_t ask = 1001;
  std::uint8_t flags = 7;
};

// Finite copied scripts and an optional always-ready tail provide deterministic
// market input without waits, callbacks, or allocation from Poll.
class QuoteSource final : public MarketDataSource {
 public:
  std::array<QuoteFrame, 16> frames = {};
  std::size_t frame_count = 0;
  bool continuous = false;
  int polls = 0;
  int stops = 0;
  base::Status Prepare(const md::MarketDataConfig& config) noexcept override {
    source_id_ = config.source_id;
    return {};
  }
  base::Status Start(std::span<const model::InstrumentId> ids,
                     std::uint64_t) noexcept override {
    active_ = !ids.empty();
    if (active_) instrument_id_ = ids[0];
    return {};
  }
  MarketDataPollResult Poll(std::uint64_t now,
                            model::QuoteTick& out) noexcept override {
    if (!active_ || (!continuous && next_ == frame_count)) return {};
    if (++polls > 1024)
      return {MarketDataPollState::kError,
              {base::ErrorCode::kInternalError, {}}};
    const auto frame =
        next_ < frame_count ? frames[next_] : frames[frame_count - 1];
    if (now < frame.at) return {};
    ++next_;
    out = {};
    out.source_id = source_id_;
    out.instrument_id = instrument_id_;
    out.sequence = next_;
    out.receive_time_ns = now;
    out.bid_price = (frame.flags & 1) ? frame.bid : 0;
    out.bid_qty = (frame.flags & 1) ? 100 : 0;
    out.ask_price = (frame.flags & 2) ? frame.ask : 0;
    out.ask_qty = (frame.flags & 2) ? 200 : 0;
    out.flags = frame.flags;
    return {MarketDataPollState::kEvent, {}};
  }
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    if (!active_ || frame_count == 0) return {};
    if (next_ < frame_count) return frames[next_].at;
    return continuous ? std::optional<std::uint64_t>{0} : std::nullopt;
  }
  base::Status Stop() noexcept override {
    ++stops;
    active_ = false;
    return {};
  }
  const char* error_detail() const noexcept override {
    return "scripted quote source failed";
  }

 private:
  model::SourceId source_id_;
  model::InstrumentId instrument_id_;
  std::size_t next_ = 0;
  bool active_ = false;
};

struct SentOrder {
  model::ClientOrderId id;
  model::OrderRequest request;
  model::BrokerOrderId broker;
  std::uint64_t at = 0;
  std::int64_t cumulative = 0;
  std::uint64_t sequence = 0;
  bool finished = false;
};
struct SentCancel {
  model::ClientOrderId id;
  model::CancelRequestId cancel_id;
  std::uint64_t at = 0;
};
struct PlannedFill {
  std::uint64_t delay = 0;
  std::int64_t quantity = 0;
};

// A bounded normalized broker used by both timer and BBO DLL tests. Accepted
// orders remain working unless an explicit fill or cancel plan terminates them.
// Histories are retained for assertions; no plan or input is borrowed.
class Broker final : public trading::TradingService {
 public:
  base::Status Init(const trading::TradingClientConfig&,
                    const md::MarketDataConfig& dictionary) noexcept override {
    instrument_ = dictionary.instruments[0].instrument;
    cash_ = {account_.cash_balance, 0, account_.cash_balance};
    position_ = {instrument_.id, 0, 0, 0};
    for (std::size_t i = 0; i < account_.position_count; ++i)
      if (account_.positions[i].instrument_id == instrument_.id)
        position_ = account_.positions[i];
    initialized_ = true;
    return {};
  }
  void Reset() noexcept override { initialized_ = false; }
  void SetAcceptingRequests(bool enabled) noexcept override {
    accepting_ = enabled && !stopping_;
  }
  void BeginStopping() noexcept override {
    stopping_ = true;
    accepting_ = false;
  }
  bool accepting_orders() const noexcept override {
    return initialized_ && accepting_ && !stopping_;
  }
  base::ErrorCode SubmitOrder(const model::OrderRequest& request,
                              model::ClientOrderId& out) noexcept override {
    if (!accepting_orders()) return base::ErrorCode::kInvalidState;
    if (count_ == snapshots_.size()) return base::ErrorCode::kResourceExhausted;
    auto& p = snapshots_[count_++];
    p.client_order_id = {count_};
    p.instrument_id = request.instrument_id;
    p.side = request.side;
    p.limit_price = request.limit_price;
    p.original_qty = p.leaves_qty = request.quantity;
    p.position_effect = request.position_effect;
    out = p.client_order_id;
    return base::ErrorCode::kOk;
  }
  base::ErrorCode CancelOrder(model::ClientOrderId id) noexcept override {
    if (!accepting_orders()) return base::ErrorCode::kInvalidState;
    if (id.value == 0 || id.value > count_)
      return base::ErrorCode::kInvalidArgument;
    auto& p = snapshots_[id.value - 1];
    if (p.state != model::OrderState::kWorking || p.cancel_pending)
      return base::ErrorCode::kInvalidState;
    p.cancel_pending = true;
    p.cancel_request_id = {++cancel_id_};
    return base::ErrorCode::kOk;
  }
  bool TryGetOrder(model::ClientOrderId id,
                   model::OrderSnapshot& out) const noexcept override {
    if (!initialized_ || id.value == 0 || id.value > count_) return false;
    out = snapshots_[id.value - 1];
    return true;
  }
  bool TryGetCash(model::CashSnapshot& out) const noexcept override {
    if (!initialized_) return false;
    out = cash_;
    return true;
  }
  bool TryGetPosition(model::InstrumentId id,
                      model::PositionSnapshot& out) const noexcept override {
    if (!initialized_ || id != instrument_.id) return false;
    out = position_;
    return true;
  }
  bool TryGetFuturesAccount(
      model::FuturesAccountSnapshot&) const noexcept override {
    return false;
  }
  bool TryGetFuturesPosition(
      model::InstrumentId,
      model::FuturesPositionSnapshot&) const noexcept override {
    return false;
  }
  base::Status OnQuote(const model::QuoteTick&) noexcept override {
    return quote_result;
  }
  base::Status quote_result;
  std::size_t order_count() const noexcept override { return count_; }
  const char* error_stage() const noexcept override { return "trading_poll"; }
  bool has_ready_event(std::uint64_t now) const noexcept override {
    const auto p = NextPlan();
    return local_count_ != 0 || (p && plans_[*p].at <= now);
  }
  base::Status ProcessRequests(std::uint64_t now,
                               std::size_t = 64) noexcept override {
    for (std::size_t i = 0; i < count_; ++i) {
      auto& p = snapshots_[i];
      if (!attempted_[i]) {
        attempted_[i] = true;
        if (stopping_) {
          p.state = model::OrderState::kCanceled;
          p.leaves_qty = 0;
          local_[local_count_++] = i;
          continue;
        }
        auto status = SendOrder(p.client_order_id,
                                {p.instrument_id, p.side, p.limit_price,
                                 p.original_qty, p.position_effect},
                                now);
        if (!status.ok()) return status;
      }
      if (stopping_ && p.state == model::OrderState::kWorking &&
          !p.cancel_pending) {
        p.cancel_pending = true;
        p.cancel_request_id = {++cancel_id_};
      }
      if (p.cancel_pending && sent_cancel_[i] != p.cancel_request_id) {
        sent_cancel_[i] = p.cancel_request_id;
        auto status = SendCancel(p.client_order_id, p.broker_order_id,
                                 p.cancel_request_id, now);
        if (!status.ok()) return status;
      }
    }
    return {};
  }
  base::Status Poll(std::uint64_t now, trading::TradingDispatch& out,
                    bool& event) noexcept override {
    if (local_count_) {
      const auto i = local_[--local_count_];
      out = {};
      out.notify_order = true;
      out.update.kind = model::OrderUpdateKind::kCanceledBeforeSend;
      out.update.order = snapshots_[i];
      event = true;
      return {};
    }
    trading::TradingEvent report;
    const auto result = Poll(now, report);
    if (!result.status.ok()) return result.status;
    if (result.state == trading::TradingPollState::kIdle) {
      event = false;
      return {};
    }
    auto& p = snapshots_[report.client_order_id.value - 1];
    p.broker_order_id = report.broker_order_id;
    p.report_sequence = report.report_sequence;
    p.receive_time_ns = now;
    if (report.kind == model::OrderUpdateKind::kAccepted)
      p.state = model::OrderState::kWorking;
    else if (report.kind == model::OrderUpdateKind::kRejected) {
      p.state = model::OrderState::kRejected;
      p.leaves_qty = 0;
    } else if (report.kind == model::OrderUpdateKind::kCanceled) {
      p.state = model::OrderState::kCanceled;
      p.leaves_qty = 0;
      p.cancel_pending = false;
    } else if (report.kind == model::OrderUpdateKind::kCancelRejected)
      p.cancel_pending = false;
    else if (report.kind == model::OrderUpdateKind::kFill) {
      p.cumulative_filled_qty = report.cumulative_filled_qty;
      p.leaves_qty = p.original_qty - p.cumulative_filled_qty;
      if (!p.leaves_qty) p.state = model::OrderState::kFilled;
      const auto qty = report.fill.quantity;
      std::int64_t scale = 1;
      for (unsigned d = instrument_.price_decimals; d < 6; ++d) scale *= 10;
      if (p.side == model::OrderSide::kBuy) {
        cash_.balance -= report.fill.price * qty * scale;
        position_.total_qty += qty;
      } else {
        cash_.balance += report.fill.price * qty * scale;
        position_.total_qty -= qty;
        position_.available_qty -= qty;
      }
      cash_.available = cash_.balance;
    }
    out = {};
    out.notify_order = true;
    out.update.order = p;
    out.update.kind = report.kind;
    out.update.cancel_request_id = report.cancel_request_id;
    out.has_fill = report.kind == model::OrderUpdateKind::kFill;
    out.fill = report.fill;
    event = true;
    return {};
  }

  std::array<SentOrder, 64> orders = {};
  std::size_t sent_count = 0;
  std::array<SentCancel, 128> cancels = {};
  std::size_t cancel_count = 0;
  std::uint64_t ack_delay = 1000;
  std::uint64_t buy_cancel_delay = 1000;
  std::uint64_t sell_cancel_delay = 1000;
  int reject_buys = 0;
  int reject_cancels = 0;
  std::array<PlannedFill, 2> first_buy_fills = {};
  std::size_t fill_count = 0;
  int fills = 0;
  int stops = 0;

  base::Status Prepare(const trading::TradingConfig& config) noexcept {
    account_ = config.initial_account;
    return {};
  }
  base::Status Start(std::uint64_t) noexcept { return {}; }
  base::Status ReadAccountSnapshot(
      trading::TradingAccountSnapshot& out) noexcept {
    out = account_;
    return {};
  }
  base::Status SendOrder(model::ClientOrderId id,
                         const model::OrderRequest& request,
                         std::uint64_t now) noexcept {
    if (sent_count == orders.size()) return Error();
    const auto index = sent_count++;
    auto& order = orders[index];
    order.id = id;
    order.request = request;
    order.at = now;
    char broker[64];
    const int size = std::snprintf(broker, sizeof(broker), "broker-%llu",
                                   static_cast<unsigned long long>(id.value));
    if (order.broker.Assign({broker, static_cast<std::size_t>(size)}) !=
        base::ErrorCode::kOk)
      return Error();
    const bool buy = request.side == model::OrderSide::kBuy;
    const bool reject = buy && reject_buys > 0;
    if (reject) --reject_buys;
    if (!Schedule(index, now + ack_delay,
                  reject ? model::OrderUpdateKind::kRejected
                         : model::OrderUpdateKind::kAccepted))
      return Error();
    if (buy && !buy_seen_) {
      buy_seen_ = true;
      if (!reject)
        for (std::size_t i = 0; i < fill_count; ++i) {
          if (!Schedule(index, now + first_buy_fills[i].delay,
                        model::OrderUpdateKind::kFill, {},
                        first_buy_fills[i].quantity))
            return Error();
        }
    }
    return {};
  }
  base::Status SendCancel(model::ClientOrderId id,
                          const model::BrokerOrderId& broker,
                          model::CancelRequestId cancel_id,
                          std::uint64_t now) noexcept {
    if (cancel_count == cancels.size()) return Error();
    cancels[cancel_count++] = {id, cancel_id, now};
    for (std::size_t index = 0; index < sent_count; ++index)
      if (orders[index].id == id && orders[index].broker == broker) {
        const bool reject = reject_cancels > 0;
        if (reject) --reject_cancels;
        const auto delay = orders[index].request.side == model::OrderSide::kBuy
                               ? buy_cancel_delay
                               : sell_cancel_delay;
        return Schedule(index, now + delay,
                        reject ? model::OrderUpdateKind::kCancelRejected
                               : model::OrderUpdateKind::kCanceled,
                        cancel_id)
                   ? base::Status{}
                   : Error();
      }
    return Error();
  }
  trading::TradingPollResult Poll(std::uint64_t now,
                                  trading::TradingEvent& out) noexcept {
    const auto selected = NextPlan();
    if (!selected || plans_[*selected].at > now) return {};
    const auto plan = plans_[*selected];
    plans_[*selected].active = false;
    auto& order = orders[plan.order];
    out = {};
    out.client_order_id = order.id;
    out.broker_order_id = order.broker;
    out.report_sequence = ++order.sequence;
    out.kind = plan.kind;
    out.cancel_request_id = plan.cancel_id;
    out.receive_time_ns = now;
    if (plan.kind == model::OrderUpdateKind::kFill) {
      order.cumulative += plan.quantity;
      if (order.cumulative == order.request.quantity) order.finished = true;
      auto& fill = out.fill;
      fill.client_order_id = order.id;
      fill.broker_order_id = order.broker;
      char execution[64];
      const int size =
          std::snprintf(execution, sizeof(execution), "execution-%llu-%llu",
                        static_cast<unsigned long long>(order.id.value),
                        static_cast<unsigned long long>(order.sequence));
      if (fill.execution_id.Assign(
              {execution, static_cast<std::size_t>(size)}) !=
          base::ErrorCode::kOk)
        return {trading::TradingPollState::kError, Error()};
      fill.instrument_id = order.request.instrument_id;
      fill.side = order.request.side;
      fill.price = order.request.limit_price;
      fill.quantity = plan.quantity;
      fill.receive_time_ns = now;
      ++fills;
    } else if (plan.kind == model::OrderUpdateKind::kCanceled) {
      if (order.finished)
        out.kind = model::OrderUpdateKind::kCancelRejected;
      else {
        order.finished = true;
        for (auto& pending : plans_)
          if (pending.active && pending.order == plan.order &&
              pending.kind == model::OrderUpdateKind::kFill)
            pending.active = false;
      }
    } else if (plan.kind == model::OrderUpdateKind::kRejected)
      order.finished = true;
    out.cumulative_filled_qty = order.cumulative;
    return {trading::TradingPollState::kEvent, {}};
  }
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    if (local_count_) return 0;
    for (std::size_t i = 0; i < count_; ++i)
      if (!attempted_[i] ||
          (snapshots_[i].cancel_pending &&
           sent_cancel_[i] != snapshots_[i].cancel_request_id) ||
          (stopping_ && snapshots_[i].state == model::OrderState::kWorking &&
           !snapshots_[i].cancel_pending))
        return 0;
    const auto plan = NextPlan();
    return plan ? std::optional<std::uint64_t>{plans_[*plan].at} : std::nullopt;
  }
  bool is_drained() const noexcept override {
    if (local_count_) return false;
    for (std::size_t i = 0; i < count_; ++i)
      if (!model::IsTerminalOrderState(snapshots_[i].state) ||
          snapshots_[i].cancel_pending)
        return false;
    if (NextPlan()) return false;
    for (std::size_t index = 0; index < sent_count; ++index)
      if (!orders[index].finished) return false;
    return true;
  }
  base::Status Stop() noexcept override {
    ++stops;
    return {};
  }
  const char* error_detail() const noexcept override {
    return "bounded test broker exhausted or received an unknown request";
  }

 private:
  bool initialized_ = false, accepting_ = false, stopping_ = false;
  model::InstrumentDefinition instrument_;
  model::CashSnapshot cash_;
  model::PositionSnapshot position_;
  std::array<model::OrderSnapshot, 64> snapshots_{};
  std::array<bool, 64> attempted_{};
  std::array<model::CancelRequestId, 64> sent_cancel_{};
  std::array<std::size_t, 64> local_{};
  std::size_t count_ = 0, local_count_ = 0;
  std::uint64_t cancel_id_ = 0;

  struct Plan {
    bool active = false;
    std::size_t order = 0;
    std::uint64_t at = 0;
    std::uint64_t registration = 0;
    model::OrderUpdateKind kind = model::OrderUpdateKind::kAccepted;
    model::CancelRequestId cancel_id;
    std::int64_t quantity = 0;
  };
  bool Schedule(std::size_t order, std::uint64_t at,
                model::OrderUpdateKind kind,
                model::CancelRequestId cancel_id = {},
                std::int64_t quantity = 0) noexcept {
    for (auto& plan : plans_)
      if (!plan.active) {
        plan = {true, order, at, ++registration_, kind, cancel_id, quantity};
        return true;
      }
    return false;
  }
  std::optional<std::size_t> NextPlan() const noexcept {
    std::optional<std::size_t> result;
    for (std::size_t index = 0; index < plans_.size(); ++index)
      if (plans_[index].active &&
          (!result || plans_[index].at < plans_[*result].at ||
           (plans_[index].at == plans_[*result].at &&
            plans_[index].registration < plans_[*result].registration)))
        result = index;
    return result;
  }
  static base::Status Error() noexcept {
    return {base::ErrorCode::kInternalError, {}};
  }
  trading::TradingAccountSnapshot account_;
  std::array<Plan, 128> plans_ = {};
  std::uint64_t registration_ = 0;
  bool buy_seen_ = false;
};

// Wait simulates either the earliest host deadline or a scheduled stop signal.
// Calls are bounded; an unscheduled infinite wait fails instead of hanging.
class Environment final : public base::EventLoopEnvironment {
 public:
  explicit Environment(Clock& clock) : clock_(clock) {}
  std::optional<std::uint64_t> stop_at;
  std::uint64_t advance_on_check = 0;
  bool running = false;
  int checks = 0;
  int waits = 0;
  std::array<std::uint64_t, 128> timeouts = {};
  base::Status CheckStop(bool& out) noexcept {
    if (++checks > 10000) return {base::ErrorCode::kInternalError, {}};
    if (running) clock_.now += advance_on_check;
    out = stop_at && clock_.now >= *stop_at;
    return {};
  }
  base::Status Wait(std::optional<std::uint64_t> timeout) noexcept {
    if (stop_at && clock_.now < *stop_at &&
        (!timeout || *timeout > *stop_at - clock_.now))
      timeout = *stop_at - clock_.now;
    if (!timeout || waits == static_cast<int>(timeouts.size()))
      return {base::ErrorCode::kInternalError, {}};
    timeouts[static_cast<std::size_t>(waits++)] = *timeout;
    clock_.now += *timeout;
    return {};
  }

 private:
  Clock& clock_;
};

inline std::string TradingJson(std::int64_t cash = 100000000000000,
                               std::int64_t total = 1000,
                               std::int64_t available = 800) {
  return R"({"adapter":"mock","mode":"working","report_interval_ms":1,)"
         R"("shutdown_timeout_ms":50,"initial_cash":)" +
         std::to_string(cash) +
         R"(,"initial_positions":[{"venue":"SSE","symbol":"600000","total_qty":)" +
         std::to_string(total) + R"(,"available_qty":)" +
         std::to_string(available) + "}]}";
}

// Supplies mandatory dependencies while configuring the independent scripted
// service.
class LoopRunner : public RunnerTestHarness {
 public:
  LoopRunner(const base::MonotonicClock& clock, Environment& environment,
             MarketDataSource& source, Broker& broker) noexcept
      : RunnerTestHarness(clock, environment, source, broker),
        broker_(broker),
        environment_(environment) {}
  base::Status Handle(StrategyOptions options) noexcept {
    trading::TradingConfig config;
    config.initial_account.cash_balance = 100000000000000;
    if (options.trading_config_path) {
      trading::TradingConfigLoader loader;
      auto status = loader.Load(options.trading_config_path);
      if (!status.ok()) return status;
      config = loader.config();
    }
    auto status = broker_.Prepare(config);
    if (!status.ok()) return status;
    options.trading_config_path = nullptr;
    return RunnerTestHarness::Handle(options);
  }
  base::Status Run() noexcept {
    environment_.running = true;
    return RunnerTestHarness::Run();
  }

 private:
  Broker& broker_;
  Environment& environment_;
};

// Reads one named integer from the final stopped line, excluding startup
// account fields with the same names and any later host diagnostics.
inline std::optional<std::int64_t> SummaryNumber(std::string_view text,
                                                 std::string_view key) {
  const auto summary = text.rfind("bbo_market_making: stopped ");
  if (summary == std::string_view::npos) return {};
  text.remove_prefix(summary);
  const auto newline = text.find('\n');
  if (newline != std::string_view::npos) text = text.substr(0, newline);
  const auto found = text.find(key);
  if (found == std::string_view::npos) return {};
  text.remove_prefix(found + key.size());
  std::int64_t value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} ? std::optional<std::int64_t>{value}
                                  : std::nullopt;
}

}  // namespace yquant::strategyd::loop_testing

#endif  // YQUANT_TESTS_STRATEGYD_STRATEGY_LOOP_TEST_SUPPORT_H_
