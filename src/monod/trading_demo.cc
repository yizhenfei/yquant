#include "trading_demo.h"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <optional>

#include "yquant/container/container.h"
#include "yquant/trading/order.h"
#include "yquant/trading/order_id_generator.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_listener.h"
#include "yquant/trading/trading_service.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::monod {
namespace {

// Names the transmission outcomes used by the executable validation scenario.
const char* FailureName(trading::FailureReason reason) {
  switch (reason) {
    case trading::FailureReason::kSendFailed:
      return "send_failed";
    default:
      return "validation_failed";
  }
}

// Writes business decisions to stdout and transmission diagnostics to stderr.
class PrintTradingEvents final : public trading::TradingListener {
 public:
  // trading::TradingListener implementation.
  void OnOrderReport(const trading::OrderReport& report) override {
    const auto& order = report.snapshot.order;
    std::printf("order caller=%" PRIu64 " sequence=%" PRIu64
                " account=%" PRIu64 " strategy=%" PRIu64 " state=%s\n",
                order.order_id.caller_service_id, order.order_id.sequence,
                order.account_id, order.strategy_id,
                report.snapshot.state == trading::OrderState::kAccepted
                    ? "accepted" : "rejected");
    ++reports;
  }
  void OnRequestFailure(const trading::RequestFailure& failure) override {
    std::fprintf(stderr, "request_failure caller=%" PRIu64 " sequence=%" PRIu64
                         " reason=%s\n",
                 failure.order_id.caller_service_id, failure.order_id.sequence,
                 FailureName(failure.reason));
    ++failures;
  }

  int reports = 0;
  int failures = 0;
};

}  // namespace

int RunTradingDemo() {
  trading::SimulatedTradingAdapter first;
  trading::SimulatedTradingAdapter second;
  PrintTradingEvents output;
  trading::TradingService service;
  trading::TradingServiceRegistry registry;
  if (!service.BindAccount(7, first) || !service.BindAccount(8, second) ||
      !registry.Register(3, service)) {
    std::fprintf(stderr, "Failed to assemble trading service\n");
    return 1;
  }
  trading::TradingClientFactory factory(registry);
  auto client = factory.Create(3);
  if (!client) {
    std::fprintf(stderr, "Failed to create trading client\n");
    return 1;
  }
  trading::OrderIdGenerator ids(100);
  const std::array<trading::SimulatedSubmit, 4> scenarios{{
      {trading::SendResult::kSent, trading::AdapterOrderState::kAccepted},
      {trading::SendResult::kSent,
       trading::AdapterOrderState::kRejected},
      {trading::SendResult::kNotSent, std::nullopt},
      {trading::SendResult::kNotSent,
       trading::AdapterOrderState::kAccepted},
  }};
  for (std::size_t index = 0; index < scenarios.size(); ++index) {
    const bool use_first = index % 2 == 0;
    (use_first ? first : second).SetNextSubmit(scenarios[index]);
    const auto id = ids.Next();
    if (!id || client->SubmitOrder(
                   {{*id, use_first ? 7U : 8U, 2, 42, trading::Side::kBuy,
                     trading::Offset::kOpen, 100.5, 3}}, output) !=
                   trading::RequestResult::kDelivered) {
      std::fprintf(stderr, "Failed to deliver trading request\n");
      return 1;
    }
  }
  container::Container container;
  if (!container.Register(service) || !container.Run([&output] {
        return output.reports + output.failures == 5;
      })) {
    std::fprintf(stderr, "Failed to run trading service\n");
    return 1;
  }
  std::printf("trading_summary reports=%d failures=%d\n",
              output.reports, output.failures);
  return output.reports == 3 && output.failures == 2 ? 0 : 1;
}

}  // namespace yquant::monod
