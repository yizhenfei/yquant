#ifndef YQUANT_TRADING_TRADING_STORAGE_H_
#define YQUANT_TRADING_TRADING_STORAGE_H_

// Defines private engine storage, staged transactions and lookup helpers shared
// with the cold checkpoint codec. Public order models belong in model/.

#include <array>

#include "yquant/trading/futures_ledger.h"
#include "yquant/trading/trading_engine.h"

namespace yquant::trading {
// Private fixed storage shared by the ledger and its cold checkpoint codec.
inline constexpr std::size_t kFillLookupCapacity = 2 * kMaxTradingFills;
inline constexpr std::size_t kBrokerLookupCapacity = 2 * kMaxTradingOrders;
inline constexpr std::size_t kOrderLookupCapacity = 2 * kMaxTradingOrders;

// Rejects unknown mutation bytes before replay or codec dispatch. Every new
// mutation kind requires an explicit decision in this shared validation.
inline bool IsValidTradingMutationKind(TradingMutationKind kind) noexcept {
  switch (kind) {
    case TradingMutationKind::kNewOrder:
    case TradingMutationKind::kCancel:
    case TradingMutationKind::kSendNew:
    case TradingMutationKind::kSendCancel:
    case TradingMutationKind::kScheduleLocalCancel:
    case TradingMutationKind::kLocalCancel:
    case TradingMutationKind::kReport:
      return true;
  }
  return false;
}

// Integral identity hashing keeps sparse cold-leased IDs independent of slots.
inline std::size_t TradingOrderHash(model::ClientOrderId order) noexcept {
  auto value = order.value;
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  value ^= value >> 31;
  return value & (kOrderLookupCapacity - 1);
}

// Open-addressed lookup hashes must match for live commits and cold rebuilds.
inline std::size_t TradingBrokerHash(
    const model::AdapterOrderId& broker) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : broker.view())
    hash = (hash ^ byte) * 1099511628211ULL;
  return hash & (kBrokerLookupCapacity - 1);
}
inline std::size_t TradingFillHash(
    model::ClientOrderId order, const model::ExecutionId& execution) noexcept {
  std::uint64_t hash =
      (14695981039346656037ULL ^ order.value) * 1099511628211ULL;
  for (const unsigned char byte : execution.view())
    hash = (hash ^ byte) * 1099511628211ULL;
  return hash & (kFillLookupCapacity - 1);
}

// Holds an unpublished request and its resource claim until journal append.
struct TradingEngine::Submission {
  model::OrderSnapshot order;
  model::CashSnapshot cash;
  model::PositionSnapshot position;
  FuturesReservation futures_reservation;
  std::int64_t notional = 0;
  std::int64_t scale = 1;
  std::size_t position_index = 0;
  bool new_position = false;
  bool futures = false;
};

// Stages one report's order and cash effects until validation and WAL append
// succeed. Owned on the stack; it neither borrows events nor allocates storage.
struct TradingEngine::ReportTransaction {
  model::OrderSnapshot next;
  TradingDispatch dispatch;
  model::CashSnapshot cash;
  model::PositionSnapshot position;
  std::int64_t reserved_cash = 0;
  std::int64_t reserved_qty = 0;
  std::uint64_t retry = 0;
  std::size_t broker_slot = 0;
  std::size_t fill_slot = 0;
  bool bind_broker = false;
};

struct TradingEngine::Storage {
  struct OrderRecord {
    model::OrderSnapshot snapshot;
    bool new_attempted = false;
    bool cancel_attempted = false;
    bool local_cancel_pending = false;
    std::uint64_t retry_not_before_ns = 0;
    std::size_t position_index = 0;
    std::int64_t price_scale = 1;
    // Remaining buy micro-yuan or sell shares, respectively. Fills release
    // their matched portion; only definitive terminal events release the rest.
    std::int64_t reserved_cash = 0;
    std::int64_t reserved_qty = 0;
    FuturesReservation futures_reservation;
  };
  enum class RequestKind { kNew, kCancel };
  struct Request {
    RequestKind kind = RequestKind::kNew;
    std::size_t order_index = 0;
    model::CancelRequestId cancel_id;
  };
  std::array<OrderRecord, kMaxTradingOrders> orders = {};
  std::array<std::uint16_t, kOrderLookupCapacity> order_lookup = {};
  std::array<model::Fill, kMaxTradingFills> fills = {};
  std::array<std::uint16_t, kFillLookupCapacity> fill_lookup = {};
  std::array<std::uint16_t, kBrokerLookupCapacity> broker_lookup = {};
  std::array<Request, kMaxTradingRequests> requests = {};
  std::array<std::size_t, kMaxLocalOrderUpdates> local_updates = {};
  std::size_t order_count = 0;
  std::size_t fill_count = 0;
  std::size_t active_count = 0;
  std::size_t pending_cancel_count = 0;
  std::size_t request_head = 0;
  std::size_t request_count = 0;
  std::size_t local_head = 0;
  std::size_t local_count = 0;
  std::uint64_t last_cancel_id = 0;
  std::uint64_t next_order_id = 1;
  std::uint64_t mutation_revision = 0;
  std::size_t scan_cursor = 0;
  bool scan_pending = false;
  bool rescan_required = false;
  std::optional<std::uint64_t> next_retry;
  model::CashSnapshot cash;
  std::array<model::PositionSnapshot, md::kMaxMarketDataInstruments> positions =
      {};
  std::size_t position_count = 0;
  model::AccountType type = model::AccountType::kCash;
  bool broker_managed = false;
  BrokerAccountSnapshot broker_account;
  FuturesLedger futures;
};

}  // namespace yquant::trading
#endif  // YQUANT_TRADING_TRADING_STORAGE_H_
