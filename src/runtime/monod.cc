#include "yquant/runtime/monod.h"

#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "yquant/container/container.h"
#include "yquant/market/market_client_factory.h"
#include "yquant/market/market_service.h"
#include "yquant/market/market_service_registry.h"
#include "yquant/strategy/strategy_service.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::runtime {
namespace {
using Clock = std::chrono::steady_clock;

// Drains pre-start quotes while the SDK logs in. Only a post-start quote can
// trigger strategy callbacks; this also bounds the SDK's waiting quote queue.
class GatedSource final : public market::QuoteSource {
 public:
  GatedSource(market::SimNowQuoteSource& source, trading::TradingService& trading,
              const MonodConfig& config) : source(source), trading(trading), config(config) {}
  // market::QuoteSource implementation.
  bool TryRead(model::Quote& out) override {
    model::Quote q;
    if (!source.TryRead(q)) return false;
    trading::FundsSnapshot funds;
    if (source.failed() || !source.ready() ||
        !trading.QueryFunds({config.account_id}, funds)) {
      return false;
    }
    if (!started) {
      started = true;
      since = Clock::now();
      std::size_t count = 0;
      for (const auto& group : config.services) count += group.instances.size();
      std::printf("monod_started services=%zu strategies=%zu account=%llu\n",
                  config.services.size(), count,
                  static_cast<unsigned long long>(config.account_id));
      std::fflush(stdout);
    }
    out = q;
    return true;
  }
  market::SimNowQuoteSource& source;
  trading::TradingService& trading;
  const MonodConfig& config;
  bool started = false;
  Clock::time_point since;
};

// Receives only the runtime's shutdown cancellations after strategy callbacks
// have been detached. Failed requests remain visible in the final active set.
class ShutdownListener final : public trading::TradingListener {
 public:
  // trading::TradingListener implementation.
  void OnRequestFailure(const trading::RequestFailure& failure) override {
    const auto it = pending.find(failure.order_id);
    if (it == pending.end() || failure.cancel_request_id != it->second.cancel_request_id) return;
    if (failure.reason == trading::FailureReason::kDuplicateCancel &&
        it->second.cancel_request_id != std::numeric_limits<std::uint64_t>::max()) {
      // Duplicate IDs are rejected locally before reaching the adapter. Preserve
      // this order's account and strategy when trying the next request ID.
      ++it->second.cancel_request_id;
      if (client->CancelOrder(it->second) == trading::RequestResult::kDelivered) return;
    }
    pending.erase(it);
  }
  void OnCancelReport(const trading::CancelReport& report) override {
    const auto it = pending.find(report.order_id);
    if (it == pending.end() || report.cancel_request_id != it->second.cancel_request_id) return;
    if (report.result != trading::CancelResult::kRejected) {
      ++canceled;
      std::printf("monod_canceled caller=%llu strategy=%llu sequence=%llu request=%llu filled=%d\n",
          static_cast<unsigned long long>(report.order_id.caller_service_id),
          static_cast<unsigned long long>(it->second.strategy_id),
          static_cast<unsigned long long>(report.order_id.sequence),
          static_cast<unsigned long long>(report.cancel_request_id), report.snapshot.filled_quantity);
    }
    pending.erase(it);
  }
  void OnOrderReport(const trading::OrderReport&) override {}
  void OnTradeReport(const trading::TradeReport&) override {}
  trading::TradingClient* client = nullptr;
  std::unordered_map<trading::OrderId, trading::CancelOrderRequest, trading::OrderIdHash> pending;
  std::size_t canceled = 0;
};

bool Disconnected(const trading::SimNowTradingAdapter& adapter) {
  const auto phase = adapter.login_status().phase;
  return phase == trading::LoginPhase::kFailed || phase == trading::LoginPhase::kDisconnected;
}

}  // namespace

int RunMonod(const MonodConfig& c, market::SimNowQuoteSource& source,
             trading::SimNowTradingAdapter& adapter, const std::function<bool()>& stop,
             std::string& error) {
  ShutdownListener shutdown;
  trading::TradingService trading;
  GatedSource gate(source, trading, c);
  market::MarketService market(gate);
  trading::TradingServiceRegistry traders;
  market::MarketServiceRegistry markets;
  if (!trading.BindAccount(c.account_id, adapter) || !traders.Register(1, trading) ||
      !markets.Register(1, market)) { error = "Service assembly failed"; return 1; }
  trading::TradingClientFactory trader_factory(traders);
  market::MarketClientFactory market_factory(markets);
  auto trader = trader_factory.Create(1, shutdown);
  std::unordered_set<std::uint32_t> instruments;
  for (const auto& [native, binding] : c.instruments) instruments.insert(binding.instrument_id);
  std::vector<std::unique_ptr<strategy::StrategyService>> services;
  std::vector<trading::StrategyOrderScope> scopes;
  services.reserve(c.services.size());
  for (const auto& group : c.services) {
    auto service = std::make_unique<strategy::StrategyService>(group.caller_service_id, market_factory, 1, trader_factory, 1);
    if (!service->Load(group.instances, error)) return 1;
    services.push_back(std::move(service));
    for (const auto& instance : group.instances) {
      scopes.push_back({group.caller_service_id, c.account_id,
                        instance.strategy_id});
    }
  }
  for (auto instrument : market.SubscribedInstruments()) {
    if (!instruments.contains(instrument)) {
      error = "Strategy subscription has no deployment instrument mapping";
      return 1;
    }
  }
  container::Container container;
  container.Register(trading);
  container.Register(market);
  const auto deadline = Clock::now() + std::chrono::seconds(c.startup_seconds);
  container.Run([&] {
    if (Disconnected(adapter)) {
      const auto& login = adapter.login_status();
      error = "Trading session failed: " + std::string(login.reason) +
              " code=" + std::to_string(login.code);
      return true;
    }
    if (source.failed()) { error = "Market session failed"; return true; }
    if (trading.AccountSyncFailed(c.account_id)) {
      error = "Account initialization failed: " + std::string(adapter.snapshot_status().reason);
      if (const auto* snapshot = adapter.snapshot()) error += snapshot->holds_error;
      return true;
    }
    if (!gate.started && Clock::now() >= deadline) {
      error = "Timed out waiting for account readiness and market data";
      return true;
    }
    return (stop && stop()) || (gate.started && c.run_seconds > 0 &&
        Clock::now() - gate.since >= std::chrono::seconds(c.run_seconds));
  });
  for (const auto& service : services) {
    if (!service->Stop()) { error = "Could not detach strategy callbacks"; return 1; }
  }
  std::vector<trading::OrderSnapshot> active, group_orders;
  const auto collect = [&] {
    active.clear();
    std::uint64_t last;
    for (const auto& scope : scopes) {
      if (!trader->QueryWorkingOrders(scope, group_orders, last)) return false;
      active.insert(active.end(), group_orders.begin(), group_orders.end());
    }
    return true;
  };
  if (!collect()) return error.empty() ? 0 : 1;
  shutdown.client = trader.get();
  shutdown.pending.reserve(active.size());
  for (const auto& snapshot : active) {
    const auto& order = snapshot.order;
    const trading::CancelOrderRequest request{order.order_id, order.account_id, order.strategy_id, 1};
    if (trader->CancelOrder(request) == trading::RequestResult::kDelivered) {
      shutdown.pending.emplace(order.order_id, request);
    }
  }
  const auto closing = Clock::now() + std::chrono::seconds(c.shutdown_seconds);
  trading::FundsSnapshot funds;
  do {
    trading.Run();
    if (!collect()) { error = "Cannot query orders during shutdown"; break; }
    if (active.empty() && shutdown.pending.empty() && trader->QueryFunds({c.account_id}, funds)) {
      std::printf("monod_stopped canceled=%zu remaining=0 commission=%.12g realized=%.12g\n",
                  shutdown.canceled, funds.commission, funds.realized_pnl);
      std::fflush(stdout);
      return error.empty() ? 0 : 1;
    }
    if (Disconnected(adapter) || (!active.empty() && shutdown.pending.empty())) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (Clock::now() < closing);
  for (const auto& snapshot : active) {
    const auto& order = snapshot.order;
    std::fprintf(stderr, "monod_unconfirmed caller=%llu strategy=%llu sequence=%llu\n",
        static_cast<unsigned long long>(order.order_id.caller_service_id),
        static_cast<unsigned long long>(order.strategy_id),
        static_cast<unsigned long long>(order.order_id.sequence));
  }
  for (const auto& [id, request] : shutdown.pending) {
    std::fprintf(stderr, "monod_cancel_unconfirmed caller=%llu strategy=%llu sequence=%llu request=%llu\n",
        static_cast<unsigned long long>(id.caller_service_id),
        static_cast<unsigned long long>(request.strategy_id),
        static_cast<unsigned long long>(id.sequence),
        static_cast<unsigned long long>(request.cancel_request_id));
  }
  error = "Shutdown could not confirm all orders and priced fills; remaining=" +
          std::to_string(active.size());
  return 1;
}

}  // namespace yquant::runtime
