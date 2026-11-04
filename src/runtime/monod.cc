#include "yquant/runtime/monod.h"

#include <chrono>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

#include "yquant/container/container.h"
#include "yquant/market/market_client_factory.h"
#include "yquant/market/market_service.h"
#include "yquant/market/market_service_registry.h"
#include "yquant/strategy/loaded_strategy.h"
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
  market::QuoteReadResult Read(model::Quote& q) override {
    const auto result = source.Read(q);
    if (result != market::QuoteReadResult::kQuote) return result;
    trading::FundsSnapshot funds;
    if (source.failed() || !trading.QueryFunds({config.strategy.account_id}, funds)) {
      return market::QuoteReadResult::kIdle;
    }
    if (!started) {
      started = true;
      since = Clock::now();
      std::printf("monod_started strategy=%llu account=%llu instrument=%u\n",
          static_cast<unsigned long long>(config.strategy.strategy_id),
          static_cast<unsigned long long>(config.strategy.account_id), config.strategy.instrument_id);
      std::fflush(stdout);
    }
    return result;
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
    if (failure.reason == trading::FailureReason::kDuplicateCancel &&
        failure.cancel_request_id && *failure.cancel_request_id !=
            std::numeric_limits<std::uint64_t>::max()) {
      // Duplicate IDs are rejected locally before reaching the adapter.
      if (client->CancelOrder({failure.order_id, account, strategy,
                               *failure.cancel_request_id + 1}, *this) !=
          trading::RequestResult::kDelivered) rejected = true;
    } else {
      rejected = true;
    }
  }
  void OnCancelReport(const trading::CancelReport& report) override {
    if (report.result == trading::CancelResult::kRejected) rejected = true;
    else ++canceled;
  }
  void OnOrderReport(const trading::OrderReport&) override {}
  void OnTradeReport(const trading::TradeReport&) override {}
  trading::TradingClient* client = nullptr;
  std::uint64_t account = 0, strategy = 0;
  std::size_t canceled = 0;
  bool rejected = false;
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
  trading::OrderIdGenerator ids(c.caller_service_id);
  if (!trading.BindAccount(c.strategy.account_id, adapter) || !traders.Register(1, trading) ||
      !markets.Register(1, market)) { error = "Service assembly failed"; return 1; }
  auto trader = trading::TradingClientFactory(traders).Create(1);
  auto quotes = market::MarketClientFactory(markets).Create(1);
  auto strategy = strategy::LoadedStrategy::Load(c.strategy, *quotes, *trader, ids, error);
  if (!strategy) return 1;
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
    if (trading.AccountSyncFailed(c.strategy.account_id)) {
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
  if (!strategy->Stop()) { error = "Could not detach strategy callbacks"; return 1; }
  const trading::StrategyOrderScope scope{c.caller_service_id, c.strategy.account_id,
                                          c.strategy.strategy_id, c.strategy.instrument_id};
  std::vector<trading::OrderSnapshot> active;
  std::uint64_t last;
  if (!trader->QueryWorkingOrders(scope, active, last)) return error.empty() ? 0 : 1;
  shutdown.client = trader.get();
  shutdown.account = scope.account;
  shutdown.strategy = scope.strategy;
  for (const auto& order : active) {
    if (trader->CancelOrder({order.order.order_id, scope.account, scope.strategy, 1}, shutdown) !=
        trading::RequestResult::kDelivered) shutdown.rejected = true;
  }
  const auto closing = Clock::now() + std::chrono::seconds(c.shutdown_seconds);
  trading::FundsSnapshot funds;
  do {
    trading.Run();
    trader->QueryWorkingOrders(scope, active, last);
    if (active.empty() && trader->QueryFunds({scope.account}, funds)) {
      std::printf("monod_stopped canceled=%zu remaining=0 commission=%.12g realized=%.12g\n",
                  shutdown.canceled, funds.commission, funds.realized_pnl);
      std::fflush(stdout);
      return error.empty() ? 0 : 1;
    }
    if (Disconnected(adapter) || shutdown.rejected) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (Clock::now() < closing);
  error = "Shutdown could not confirm all orders and priced fills; remaining=" +
          std::to_string(active.size());
  return 1;
}

}  // namespace yquant::runtime
