#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <thread>

#include "yquant/trading/order_id_generator.h"
#include "yquant/trading/simnow_trading_adapter.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_service_registry.h"

namespace {
using namespace yquant::trading;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kAccount = 1, kCaller = 710011, kStrategy = 11;

std::string_view Env(const char* key) {
  auto* value = std::getenv(key);
  return value ? value : "";
}

template <typename T>
bool Parse(std::string_view arg, T& out) {
  if (arg.empty()) return false;
  auto [end, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), out);
  return ec == std::errc{} && end == arg.data() + arg.size();
}

// Counts priced, deduplicated executions delivered to this exact invocation.
// The service owns all booking; the probe never simulates a counter report.
class Results final : public TradingListener {
 public:
  OrderInfo order;
  int filled = 0;
  std::vector<TradeInfo> fills;
  bool canceled = false, failed = false;
  // TradingListener implementation.
  void OnTradeReport(const TradeReport& r) override {
    if (r.trade.order_id != order.order_id || r.snapshot.order != order) { failed = true; return; }
    filled += r.trade.quantity;
    fills.push_back(r.trade);
    std::printf("trade id=%s price=%.12g lots=%d\n", r.trade.trade_id.value.c_str(),
                r.trade.price, r.trade.quantity);
  }
  void OnOrderReport(const OrderReport& r) override {
    if (r.snapshot.order != order || r.snapshot.state == OrderState::kRejected) failed = true;
    std::printf("order_report state=%d filled=%d\n", int(r.snapshot.state), r.snapshot.filled_quantity);
  }
  void OnCancelReport(const CancelReport& r) override {
    canceled = r.order_id == order.order_id && r.cancel_request_id == 1 &&
               r.result == CancelResult::kCanceled && r.snapshot.order == order;
    if (!canceled) failed = true;
  }
  void OnRequestFailure(const RequestFailure& r) override {
    failed = true;
    std::fprintf(stderr, "request_failed reason=%d\n", int(r.reason));
  }
};

bool Failed(const SimNowTradingAdapter& a) {
  const auto& login = a.login_status();
  const auto q = a.snapshot_status();
  if (login.phase != LoginPhase::kFailed && login.phase != LoginPhase::kDisconnected &&
      q.phase != SnapshotPhase::kFailed) return false;
  std::fprintf(stderr, "session_failed login=%s code=%d snapshot=%s query_code=%d\n",
               login.reason, login.code, q.reason, q.code);
  return true;
}

struct LocalResult {
  std::string day;
  SimNowNativeOrderKey native{};
  FundsSnapshot funds;
  std::vector<PositionInfo> positions;
  Results events;
};

int SendAndCancel(const SimNowTraderConfig& config, OrderInfo order, LocalResult& local) {
  auto adapter = SimNowTradingAdapter::Connect(config);
  Results& result = local.events;
  TradingService service;
  TradingServiceRegistry registry;
  if (!adapter || !service.BindAccount(kAccount, *adapter) || !registry.Register(1, service)) return 1;
  auto client = TradingClientFactory(registry).Create(1);
  const auto startup_deadline = Clock::now() + 65s;
  while (!service.IsAccountReady(kAccount)) {
    service.Run();
    if (Failed(*adapter) || service.AccountSyncFailed(kAccount) || Clock::now() >= startup_deadline) return 1;
    std::this_thread::sleep_for(1ms);
  }
  FundsSnapshot funds;
  PositionsSnapshot positions;
  if (!client->QueryFunds({kAccount}, funds) || !client->QueryPositions({kAccount}, positions)) return 1;
  const auto* snapshot = adapter->snapshot();
  if (!snapshot) return 1;
  local.day = snapshot->trading_day;
  OrderIdGenerator ids(kCaller, snapshot->history.LastSequence(kCaller));
  const auto id = ids.Next();
  if (!id) return 1;
  order.order_id = *id;
  result.order = order;
  std::printf("sending caller=%llu sequence=%llu price=%.12g lots=%d\n",
      static_cast<unsigned long long>(id->caller_service_id),
      static_cast<unsigned long long>(id->sequence), order.price, order.quantity);
  std::fflush(stdout);
  if (client->SubmitOrder({order}, result) != RequestResult::kDelivered) return 1;
  const auto start = Clock::now();
  bool cancel_sent = false;
  const QueryOrderRequest key{*id, kAccount, kStrategy};
  OrderSnapshot state;
  for (;;) {
    service.Run();
    if (Failed(*adapter) || result.failed) {
      std::fprintf(stderr, "order_verification_failed caller=%llu sequence=%llu cancel_sent=%d\n",
                   static_cast<unsigned long long>(id->caller_service_id),
                   static_cast<unsigned long long>(id->sequence), cancel_sent);
      return 1;
    }
    if (client->QueryOrder(key, state)) {
      if (state.order != order) return 1;
      if (state.state == OrderState::kFilled || state.state == OrderState::kCanceled) {
        if (result.filled == state.filled_quantity &&
            (state.state == OrderState::kFilled || result.canceled)) break;
      } else if (!cancel_sent && (state.state == OrderState::kAccepted ||
                 state.state == OrderState::kPartiallyFilled || Clock::now() - start > 10s)) {
        cancel_sent = true;
        if (client->CancelOrder({*id,kAccount,kStrategy,1}, result) != RequestResult::kDelivered) return 1;
        std::puts("cancel_requested");
        std::fflush(stdout);
      }
    }
    if (Clock::now() - start > 45s) {
      std::fprintf(stderr, "terminal_unconfirmed sequence=%llu\n",
                   static_cast<unsigned long long>(id->sequence));
      return 1;
    }
    std::this_thread::sleep_for(1ms);
  }
  FundsSnapshot final;
  if (!client->QueryFunds({kAccount}, final) || !client->QueryPositions({kAccount}, positions)) return 1;
  std::printf("local_terminal state=%s filled=%d commission=%.12g realized=%.12g\n",
      result.canceled ? "canceled" : "filled", result.filled, final.commission, final.realized_pnl);
  std::fflush(stdout);

  const auto native = adapter->NativeKey(*id);
  if (!native) return 1;
  local.native = *native;
  std::printf("native_binding front=%d session=%d ref=%s system_id=%s\n",
              native->front, native->session, native->ref.c_str(), native->system_id.c_str());
  local.funds = final;
  local.positions.assign(positions.positions.begin(), positions.positions.end());
  return 0;
}

// Keep the second session alive while comparing the immutable query result.
std::unique_ptr<SimNowTradingAdapter> ReadBack(const SimNowTraderConfig& config) {
  auto reader = SimNowTradingAdapter::Connect(config);
  if (!reader) return nullptr;
  bool requested = false;
  const auto deadline = Clock::now() + 65s;
  while (Clock::now() < deadline) {
    reader->Run();
    if (Failed(*reader)) return nullptr;
    if (!requested && reader->login_status().phase == LoginPhase::kLoggedIn) {
      requested = reader->StartSnapshot();
      if (!requested) return nullptr;
    }
    if (reader->snapshot()) return reader;
    std::this_thread::sleep_for(1ms);
  }
  std::fputs("readback_timeout\n", stderr);
  return nullptr;
}

// Prints matching native records without inferring ownership from matching
// order parameters. This path is read-only, including settlement handling.
int Inspect(const SimNowTraderConfig& config, const OrderInfo& order,
            std::string_view exchange, std::string_view symbol) {
  auto reader = ReadBack(config);
  if (!reader) return 1;
  const auto* snapshot = reader->snapshot();
  int matches = 0;
  for (const auto& row : snapshot->history.orders()) {
    if (row.exchange != exchange || row.symbol != symbol || row.price != order.price ||
        row.quantity != order.quantity || row.direction != (order.side == Side::kBuy ? '0' : '1')) continue;
    std::printf("native day=%s exchange=%s symbol=%s system_id=%s ref=%s front=%d session=%d "
                "status=%c traded=%d tag=%s\n", snapshot->trading_day.c_str(), row.exchange.c_str(),
                row.symbol.c_str(), row.system_id.c_str(), row.ref.c_str(), row.front, row.session,
                row.status, row.traded, row.owner ? "decoded" : "absent_or_invalid");
    if (row.owner) std::printf("owner caller=%llu sequence=%llu strategy=%llu\n",
        static_cast<unsigned long long>(row.owner->order_id.caller_service_id),
        static_cast<unsigned long long>(row.owner->order_id.sequence),
        static_cast<unsigned long long>(row.owner->strategy_id));
    ++matches;
  }
  std::printf("readback matches=%d holds=%s\n", matches,
              snapshot->holds_error.empty() ? "verified" : snapshot->holds_error.c_str());
  return matches == 1 ? 0 : 1;
}

int Verify(const SimNowTraderConfig& config, OrderInfo order) {
  LocalResult local;
  if (SendAndCancel(config, order, local)) return 1;
  const auto& result = local.events;
  // Open a separate read-only session to verify the counter's saved order tag
  // and fills. Never resubmit, even if verification after execution fails.
  auto readback = ReadBack(config);
  if (!readback) return 1;
  const auto* observed = readback->snapshot();
  const auto* saved = observed->history.Find(local.native.front, local.native.session, local.native.ref);
  if (!observed->holds_error.empty() || !saved || saved->quantity != order.quantity ||
      saved->traded != result.filled || !observed->history.HasCompleteFills() ||
      saved->system_id != local.native.system_id || saved->exchange != local.native.exchange ||
      saved->price != order.price ||
      observed->trading_day != local.day) return 1;
  if (result.canceled && saved->status != '5' && saved->status != '4' && saved->status != '2') return 1;
  if (!result.canceled && saved->status != '0') return 1;
  AccountRecovery actual;
  if (!BuildAccountRecovery(*observed, config.instruments, actual).empty() ||
      std::abs(actual.funds.balance - local.funds.funds.balance) >= 0.005 ||
      std::abs(actual.funds.margin - local.funds.funds.margin) >= 0.005 ||
      std::abs(actual.funds.frozen_margin - local.funds.funds.frozen_margin) >= 0.005) return 1;
  for (const auto& trade : result.fills) {
    auto it = observed->history.fills().find(trade.trade_id);
    if (it == observed->history.fills().end() || it->second.price != trade.price ||
        it->second.quantity != trade.quantity ||
        &observed->history.orders()[it->second.order_index] != saved) return 1;
  }
  if (std::abs(actual.funds.available - local.funds.funds.available) >= 0.005) return 1;
  auto expected = local.positions;
  std::erase_if(expected, [](const auto& p) { return !p.today_quantity && !p.yesterday_quantity; });
  if (actual.positions.size() != expected.size()) return 1;
  for (const auto& p : actual.positions) {
    auto it = std::find_if(expected.begin(), expected.end(), [&](const auto& e) { return e.key == p.key; });
    if (it == expected.end() || it->today_quantity != p.today_quantity ||
        it->yesterday_quantity != p.yesterday_quantity || it->today_frozen != p.today_frozen ||
        it->yesterday_frozen != p.yesterday_frozen) return 1;
  }
  std::printf("counter_verified trading_day=%s exchange=%s symbol=%s system_id=%s filled=%d\n",
      observed->trading_day.c_str(), saved->exchange.c_str(), saved->symbol.c_str(),
      saved->system_id.c_str(), saved->traded);
  if (!saved->owner || saved->owner->order_id != result.order.order_id ||
      saved->owner->strategy_id != kStrategy) {
    std::fputs("ownership_tag_not_verified\n", stderr);
    return 4;
  }
  return result.canceled ? 0 : 3;
}
}  // namespace

int main(int argc, char** argv) {
  const bool inspect = argc > 1 && std::string_view(argv[1]) == "--inspect";
  if (inspect) { --argc; ++argv; }
  if (argc < 8 || argc % 2 != 0) {
    std::fputs("Usage: verify_simnow_trading [--inspect] FRONT EXCHANGE SYMBOL buy|sell "
               "open|close|today|yesterday PRICE LOTS [EXCHANGE SYMBOL]...\n", stderr);
    return 2;
  }
  const std::string_view front = argv[1], direction = argv[4], offset = argv[5];
  if (front != "tcp://182.254.243.31:30001" && front != "tcp://182.254.243.31:30002" &&
      front != "tcp://182.254.243.31:30003" && front != "tcp://182.254.243.31:40001") return 2;
  OrderInfo o{{},kAccount,kStrategy,1};
  if (!Parse(argv[6], o.price) || !std::isfinite(o.price) || o.price <= 0 ||
      !Parse(argv[7], o.quantity) || o.quantity <= 0) return 2;
  if (direction == "buy") o.side = Side::kBuy;
  else if (direction == "sell") o.side = Side::kSell;
  else return 2;
  if (offset == "open") o.offset = Offset::kOpen;
  else if (offset == "close") o.offset = Offset::kClose;
  else if (offset == "today") o.offset = Offset::kCloseToday;
  else if (offset == "yesterday") o.offset = Offset::kCloseYesterday;
  else return 2;
  SimNowTraderConfig c{front, Env("YQUANT_CTP_BROKER_ID"), Env("YQUANT_CTP_USER_ID"),
      Env("YQUANT_CTP_PASSWORD"), Env("YQUANT_CTP_APP_ID"), Env("YQUANT_CTP_AUTH_CODE"), {}, 45s};
  if (c.broker != "9999") return 2;
  c.confirm_settlement = !inspect;
  c.instruments[{argv[2],argv[3]}] = {1};
  for (int i = 8; i < argc; i += 2) {
    const auto added = c.instruments.emplace(std::pair{std::string(argv[i]), std::string(argv[i+1])},
        SimNowInstrumentBinding{static_cast<std::uint32_t>(i / 2)});
    if (!added.second) return 2;
  }
  std::string info;
  if (const char* path = std::getenv("YQUANT_CTP_SYSTEM_INFO_FILE")) {
    std::ifstream f(path, std::ios::binary);
    info.assign(std::istreambuf_iterator<char>(f), {});
    if (info.empty() || f.bad()) return 2;
    c.system_info = info;
  }
  return inspect ? Inspect(c, o, argv[2], argv[3]) : Verify(c, o);
}
