#ifndef YQUANT_TRADINGD_ARCHIVE_PROJECTION_H_
#define YQUANT_TRADINGD_ARCHIVE_PROJECTION_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "strategyd/market_data.h"
#include "strategyd/trading.h"
#include "tradingd/config.h"

namespace yquant::tradingd {

// Retained identity in the original gateway slot. Empty slots have zero IDs.
struct ArchiveSession {
  std::size_t slot = 0;
  std::uint64_t client_id = 0;
  std::uint64_t session_id = 0;
  std::array<InstrumentSpec, kMaxInstruments> instruments{};
  std::size_t instrument_count = 0;
};

// SQL business identity is (session_id, local_order_id); core_id is zero for
// gateway rejections. snapshot retains gateway-core order IDs for accepted
// orders, while report_sequence and local_cancel_id use client-visible IDs.
struct ArchiveOrder {
  std::uint64_t client_id = 0;
  std::uint64_t session_id = 0;
  std::uint64_t local_order_id = 0;
  model::ClientOrderId core_id;
  model::OrderSnapshot snapshot;
  std::uint64_t report_sequence = 0;
  model::CancelRequestId local_cancel_id;
  // Gateway ownership mapping for translating subsequent cancel callbacks.
  model::CancelRequestId core_cancel_id;
  std::uint64_t source_lsn = 0;
};

// Last replayed client-visible callback, valid until the next replay/restore.
struct ArchiveOrderUpdate {
  std::uint64_t client_id = 0;
  std::uint64_t session_id = 0;
  std::uint64_t local_order_id = 0;
  model::OrderUpdate update;
  bool notify_order = true;
  bool has_fill = false;
  model::Fill fill;
};

// One worker owns the projection and all access to it. Configure allocates
// bounded storage once; restore/replay never call a broker or allocate growing
// collections. A failed restore invalidates this projection; discard it.
class ArchiveProjection {
 public:
  // Borrows clock for this object's lifetime; callbacks never read live time
  // during replay because each ledger fact supplies its original timestamp.
  explicit ArchiveProjection(const strategyd::MonotonicClock& clock) noexcept;
  ~ArchiveProjection() noexcept;
  ArchiveProjection(const ArchiveProjection&) = delete;
  ArchiveProjection& operator=(const ArchiveProjection&) = delete;

  // Copies validated startup values and configures the shadow cash ledger.
  // Call once before Restore; failure leaves this object unconfigured.
  [[nodiscard]] base::Status Configure(const ServerConfig& config) noexcept;
  // Loads archive baseline v1 and gateway checkpoint v1. The baseline's run
  // and account must match its metadata and checkpoint. Calls only when idle.
  [[nodiscard]] base::Status Restore(
      std::span<const std::byte> archive_baseline) noexcept;
  // Accepts identity v2 updates only when existing identities/scope and the
  // immutable catalog are unchanged. Failure preserves retained sessions.
  [[nodiscard]] base::Status LoadIdentity(
      std::span<const std::byte> identity) noexcept;
  // Requires the next LSN. Ledger and rejection records advance projections;
  // mock records are validated and advance progress without changing accounts.
  // A failed replay must stop the consumer; discard/rebuild before retrying.
  [[nodiscard]] base::Status Replay(
      std::uint16_t type, std::uint64_t lsn,
      std::span<const std::byte> payload) noexcept;

  const std::array<std::uint64_t, 2>& ledger_id() const noexcept {
    return ledger_id_;
  }
  std::uint64_t run_id() const noexcept { return run_id_; }
  std::string_view account_key() const noexcept { return account_key_; }
  std::uint64_t last_lsn() const noexcept { return last_lsn_; }
  std::uint64_t last_record_time() const noexcept { return last_record_time_; }
  std::span<const ArchiveSession> sessions() const noexcept;
  std::span<const InstrumentSpec> instruments() const noexcept;
  std::span<const ArchiveOrder> orders() const noexcept;
  const strategyd::TradingImpl& trading() const noexcept { return trading_; }
  // Returns nullptr for unknown/zero core identities.
  const ArchiveOrder* FindOrder(model::ClientOrderId core_id) const noexcept;
  // Retained fill indices share TradingImpl::TryGetFillAt's ordering. Baseline
  // fills use zero; later fills retain their introducing record LSN. Invalid
  // indices return zero.
  std::uint64_t FillSourceLsn(std::size_t index) const noexcept;
  bool has_update() const noexcept;
  // Requires has_update(). The result lives until the next replay/restore.
  const ArchiveOrderUpdate& last_update() const noexcept;

 private:
  struct Storage;
  [[nodiscard]] base::Status RestoreGateway(
      std::span<const std::byte> checkpoint) noexcept;
  [[nodiscard]] base::Status RefreshOrders() noexcept;
  strategyd::MarketDataImpl market_data_;
  strategyd::TradingImpl trading_;
  std::unique_ptr<Storage> storage_;
  std::array<std::uint64_t, 2> ledger_id_{};
  std::uint64_t run_id_ = 0;
  std::uint64_t last_lsn_ = 0;
  std::uint64_t last_record_time_ = 0;
  char account_key_[kMaxAccountKeyBytes + 1]{};
};

}  // namespace yquant::tradingd
#endif  // YQUANT_TRADINGD_ARCHIVE_PROJECTION_H_
