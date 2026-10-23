#ifndef YQUANT_TRADING_CTP_TRADING_ADAPTER_H_
#define YQUANT_TRADING_CTP_TRADING_ADAPTER_H_

#include <memory>

#include "yquant/md/instrument_scope.h"
#include "yquant/model/trading_event.h"
#include "yquant/trading/trading_adapter.h"

namespace yquant::trading {

// Connects tradingd to a local SimNow SDK bridge. The bridge supplies broker
// account facts and normalized executions; this adapter never matches quotes
// or calculates simulated fees/margin. All calls are serialized by tradingd.
// A lost connection or invalid report is terminal: requests are not replayed.
class CtpTradingAdapter final : public TradingAdapter {
 public:
  // Borrows an initialized, immutable scope until destruction. Construction
  // does not connect or allocate transport storage.
  explicit CtpTradingAdapter(const md::InstrumentScope& instruments) noexcept;
  ~CtpTradingAdapter() noexcept override;
  [[nodiscard]] base::Status Prepare(
      const TradingConfig& config) noexcept override;
  // Bounded cold-path handshake. The external SDK must finish account queries
  // before the bridge sends Welcome; failure leaves requests disabled.
  [[nodiscard]] base::Status Start(std::uint64_t now) noexcept override;
  [[nodiscard]] base::Status ReadAccountSnapshot(
      TradingAccountSnapshot& out) noexcept override;
  // Copies the account paired with the last welcome/report/account message.
  // Failure preserves out; no query or allocation occurs in this method.
  [[nodiscard]] base::Status ReadBrokerAccountSnapshot(
      BrokerAccountSnapshot& out) noexcept override;
  [[nodiscard]] base::Status SendOrder(model::ClientOrderId id,
                                       const model::OrderRequest& request,
                                       std::uint64_t now) noexcept override;
  [[nodiscard]] base::Status SendCancel(
      model::ClientOrderId id, const model::AdapterOrderId& adapter_order_id,
      model::CancelRequestId cancel_request_id,
      std::uint64_t now) noexcept override;
  // Poll sends Stop only after all local orders settle, then exposes final
  // account messages before acknowledging the bridge's Drained response.
  void BeginStopping() noexcept override;
  [[nodiscard]] TradingPollResult Poll(
      std::uint64_t now, model::TradingEvent& out) noexcept override;
  std::optional<std::uint64_t> next_deadline() const noexcept override;
  bool is_drained() const noexcept override;
  // Releases transport storage. Normal drain confirmation is driven by
  // BeginStopping/Poll/is_drained; a direct Stop is not that confirmation.
  [[nodiscard]] base::Status Stop() noexcept override;
  const char* error_detail() const noexcept override;

 private:
  struct Storage;
  // Retains the first terminal transport/protocol failure until next Prepare.
  [[nodiscard]] base::Status Fail(base::ErrorCode code,
                                  const char* detail) noexcept;
  const md::InstrumentScope& instruments_;
  std::unique_ptr<Storage> storage_;
  char error_detail_[256] = {};
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_CTP_TRADING_ADAPTER_H_
