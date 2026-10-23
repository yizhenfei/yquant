#ifndef YQUANT_TRADING_CTP_TRADING_ADAPTER_H_
#define YQUANT_TRADING_CTP_TRADING_ADAPTER_H_

#include <stdint.h>

#include <memory>
#include <optional>

#include "yquant/md/instrument_scope.h"
#include "yquant/model/trading_event.h"
#include "yquant/trading/protocol.h"
#include "yquant/trading/trading_adapter.h"

namespace yquant::trading {

// Connects tradingd to a local SimNow SDK bridge. The bridge supplies broker
// account facts and normalized executions; this adapter never matches quotes
// or calculates simulated fees/margin. All calls are serialized by tradingd.
// A lost connection or invalid report is terminal: requests are not replayed.
class CtpTradingAdapter final : public TradingAdapter {
 public:
  CtpTradingAdapter(const CtpTradingAdapter&) = delete;
  CtpTradingAdapter& operator=(const CtpTradingAdapter&) = delete;

  // Borrows a non-null, initialized, immutable scope until destruction.
  // Construction does not connect or allocate transport storage.
  explicit CtpTradingAdapter(const md::InstrumentScope* instruments) noexcept;

  ~CtpTradingAdapter() noexcept override;
  [[nodiscard]] base::Status Prepare(
      const TradingConfig& config) noexcept override;
  // Bounded cold-path handshake. The external SDK must finish account queries
  // before the bridge sends Welcome; failure leaves requests disabled.
  [[nodiscard]] base::Status Start(uint64_t now) noexcept override;
  [[nodiscard]] base::Status ReadAccountSnapshot(
      TradingAccountSnapshot& out) noexcept override;
  // Copies the account paired with the last welcome/report/account message.
  // Failure preserves out; no query or allocation occurs in this method.
  [[nodiscard]] base::Status ReadBrokerAccountSnapshot(
      BrokerAccountSnapshot& out) noexcept override;
  [[nodiscard]] base::Status SendOrder(model::ClientOrderId id,
                                       const model::OrderRequest& request,
                                       uint64_t now) noexcept override;
  [[nodiscard]] base::Status SendCancel(
      model::ClientOrderId id, const model::AdapterOrderId& adapter_order_id,
      model::CancelRequestId cancel_request_id, uint64_t now) noexcept override;
  // Poll sends Stop only after all local orders settle, then exposes final
  // account messages before acknowledging the bridge's Drained response.
  void BeginStopping() noexcept override;
  [[nodiscard]] TradingPollResult Poll(
      uint64_t now, model::TradingEvent& out) noexcept override;
  std::optional<uint64_t> next_deadline() const noexcept override;
  bool is_drained() const noexcept override;
  // Releases transport storage. Normal drain confirmation is driven by
  // BeginStopping/Poll/is_drained; a direct Stop is not that confirmation.
  [[nodiscard]] base::Status Stop() noexcept override;
  const char* error_detail() const noexcept override;

 private:
  struct Storage;
  // Sends due Stop/heartbeat frames and checks the peer's receive deadline.
  base::Status ServiceConnection(uint64_t now) noexcept;
  // Validates and installs a broker account or order message; idle preserves
  // out.
  TradingPollResult ApplyBridgeMessage(const Message& message, uint64_t now,
                                       model::TradingEvent& out) noexcept;
  // Normalizes one order report and publishes it with its paired account view.
  TradingPollResult ApplyOrderMessage(const Message& message,
                                      const BrokerAccountSnapshot& account,
                                      uint64_t now,
                                      model::TradingEvent& out) noexcept;
  // Retains the first terminal transport/protocol failure until next Prepare.
  [[nodiscard]] base::Status Fail(base::ErrorCode code,
                                  const char* detail) noexcept;
  const md::InstrumentScope& instruments_;
  std::unique_ptr<Storage> storage_;
  char error_detail_[256] = {};
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_CTP_TRADING_ADAPTER_H_
