#ifndef YQUANT_TRADINGD_RECOVERY_H_
#define YQUANT_TRADINGD_RECOVERY_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "strategyd/trading_limits.h"
#include "tradingd/protocol.h"

namespace yquant::tradingd {

inline constexpr std::size_t kExternalRequestKeyBytes = 28;
inline constexpr std::size_t kMaxRecoveredBrokerOrders =
    strategyd::kMaxTradingOrders;
inline constexpr std::size_t kMaxRecoveredBrokerFills =
    strategyd::kMaxTradingFills;
inline constexpr std::size_t kMaxRecoveryIdentities = 1024;
// Queries may overlap once at page boundaries or between history and live data.
inline constexpr std::size_t kMaxRecoveryOrderFacts =
    2 * kMaxRecoveredBrokerOrders;
inline constexpr std::size_t kMaxRecoveryFillFacts =
    2 * kMaxRecoveredBrokerFills;

enum class ExternalRequestKind : std::uint8_t { kNewOrder = 1, kCancel = 2 };

// Durable namespace identifies one registered strategy session or gateway run;
// sequence is local to that namespace and kind. Neither field may be zero.
struct ExternalRequestKey {
  std::uint64_t namespace_id = 0;
  ExternalRequestKind kind = ExternalRequestKind::kNewOrder;
  std::uint64_t sequence = 0;
  friend bool operator==(const ExternalRequestKey&,
                         const ExternalRequestKey&) noexcept = default;
};

// Canonical unpadded Base32 of namespace/kind/sequence in network byte order.
// Encoding writes a terminator; decoding accepts exactly 28 uppercase bytes.
// Failure preserves output. No hash/truncation or process-local ID is involved.
[[nodiscard]] base::Status EncodeExternalRequestKey(
    std::uint64_t namespace_id, ExternalRequestKind kind,
    std::uint64_t sequence,
    char (&output)[kExternalRequestKeyBytes + 1]) noexcept;
[[nodiscard]] base::Status DecodeExternalRequestKey(
    std::string_view input, ExternalRequestKey& output) noexcept;

// Cold-synced registration scoped to the account being recovered. Namespaces
// are distinct; client IDs may recur in separate historical sessions.
struct DurableStrategyIdentity {
  std::uint64_t namespace_id = 0;
  std::uint64_t client_id = 0;
};

// Complete latest state at the view's cut, including fully terminal orders.
// fact_sequence is the broker's stable last-change sequence, not a page number.
struct BrokerRecoveryOrder {
  model::BrokerOrderId request_key;
  model::BrokerOrderId broker_order_id;
  model::OrderRequest request;
  model::OrderState state = model::OrderState::kPendingNew;
  std::int64_t cumulative_filled_qty = 0;
  std::uint64_t fact_sequence = 0;
};

// One broker execution, independent of gateway-local core IDs and monotonic
// clocks. Its identity is (request_key, execution_id). Repeated query pages
// must retain the same originating fact_sequence and exact execution fields.
struct BrokerRecoveryFill {
  model::BrokerOrderId request_key;
  model::BrokerOrderId broker_order_id;
  model::ExecutionId execution_id;
  model::InstrumentId instrument_id;
  model::OrderSide side = model::OrderSide::kUnknown;
  std::int64_t price = 0;
  std::int64_t quantity = 0;
  std::uint64_t event_time_ns = 0;
  bool has_event_time = false;
  std::uint64_t fact_sequence = 0;
  model::PositionEffect position_effect = model::PositionEffect::kNone;
};

// Adapter-owned query result, borrowed only during planning. A complete view
// contains all active orders plus every order changed/terminated since the
// inclusive history_lower_bound, and lifetime execution details for every
// included order (older fills may precede that lower bound). The adapter must
// establish a query barrier after fencing the old sender; all facts and account
// describe the same consistent_cut. Merely receiving the last page is not a
// consistency barrier. A partial/active-only query must leave flags false.
struct BrokerRecoveryView {
  std::string_view account_key;
  std::span<const BrokerRecoveryOrder> orders;
  std::span<const BrokerRecoveryFill> fills;
  AccountView account;
  std::uint64_t history_lower_bound = 0;
  std::uint64_t consistent_cut = 0;
  bool old_sender_fenced = false;
  bool history_complete = false;
  bool terminal_history_complete = false;
  bool account_at_cut = false;
};

struct BrokerRecoveryRequirements {
  std::string_view account_key;
  // Earliest broker sequence needed from the last fully reconciled anchor.
  // Use zero for an account whose complete lifetime history is required.
  std::uint64_t history_lower_bound = 0;
};

struct RecoveredBrokerOrder {
  ExternalRequestKey identity;
  std::uint64_t client_id = 0;
  BrokerRecoveryOrder fact;
};

struct RecoveredBrokerFill {
  std::size_t order_index = 0;
  BrokerRecoveryFill fact;
};

// Caller-owned fixed output, suitable for preallocation at initialization.
// account is the new financial baseline at consistent_cut. Orders/fills only
// restore ownership/status/dedup evidence; historical fills must not debit this
// account again. Old strategy sessions remain invalid and callbacks are not
// resumed. Active orders still require a separate risk-reducing cleanup phase.
struct RecoveredBrokerState {
  AccountView account;
  std::uint64_t history_lower_bound = 0;
  std::uint64_t consistent_cut = 0;
  std::array<RecoveredBrokerOrder, kMaxRecoveredBrokerOrders> orders;
  std::array<RecoveredBrokerFill, kMaxRecoveredBrokerFills> fills;
  std::size_t order_count = 0;
  std::size_t fill_count = 0;
};

enum class BrokerRecoveryError {
  kNone,
  kInvalidRequirements,
  kAccountMismatch,
  kSenderNotFenced,
  kIncompleteHistory,
  kCapacityExceeded,
  kInvalidIdentity,
  kUnknownOwner,
  kConflictingOrder,
  kConflictingFill,
  kInvalidFact
};

// Cold-path, bounded, allocation-free cash/equity reconciliation; futures
// position effects are rejected. Validates account units,
// dictionary membership, exact duplicates, complete per-order fill totals and
// durable ownership even when no local WAL record survived for an order.
// Preserves output on every failure; error is optional and is always assigned.
// It trusts the adapter's explicit completeness/barrier claims, which cannot be
// inferred from the returned rows. Uses at most 160 KiB of bounded stack work.
[[nodiscard]] base::Status BuildRecoveredBrokerState(
    const BrokerRecoveryView& view,
    const BrokerRecoveryRequirements& requirements,
    std::span<const DurableStrategyIdentity> identities,
    std::span<const InstrumentSpec> instruments, RecoveredBrokerState& output,
    BrokerRecoveryError* error = nullptr) noexcept;

}  // namespace yquant::tradingd

#endif  // YQUANT_TRADINGD_RECOVERY_H_
