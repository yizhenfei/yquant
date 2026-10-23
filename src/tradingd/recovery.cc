#include "tradingd/recovery.h"

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include "tradingd/config.h"

namespace yquant::tradingd {
namespace {

constexpr size_t kRequestIdentityBytes = 17;
constexpr size_t kOrderBuckets = 2 * kMaxRecoveredBrokerOrders;
constexpr size_t kFillBuckets = 2 * kMaxRecoveredBrokerFills;
constexpr uint32_t kMaxSymbolValue = model::identifier_detail::kEquitySymbolMax;
constexpr char kBase32Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
static_assert((kOrderBuckets & (kOrderBuckets - 1)) == 0);
static_assert((kFillBuckets & (kFillBuckets - 1)) == 0);
static_assert(kMaxRecoveredBrokerFills < UINT16_MAX);
static_assert(kMaxRecoveryIdentities <= UINT16_MAX);

bool ValidKind(ExternalRequestKind kind) noexcept {
  return kind == ExternalRequestKind::kNewOrder ||
         kind == ExternalRequestKind::kCancel;
}

uint64_t Hash(std::string_view value,
              uint64_t hash = 14695981039346656037ULL) noexcept {
  for (const unsigned char byte : value) {
    hash = (hash ^ byte) * 1099511628211ULL;
  }
  return hash;
}

bool ValidInstrument(model::InstrumentId id) noexcept {
  const auto venue = id.value >> 32;
  return venue >= static_cast<uint64_t>(model::Venue::kSse) &&
         venue <= static_cast<uint64_t>(model::Venue::kBse) &&
         (id.value & UINT32_MAX) <= kMaxSymbolValue;
}

bool ValidSide(model::OrderSide side) noexcept {
  return side == model::OrderSide::kBuy || side == model::OrderSide::kSell;
}

const trading::InstrumentSpec* FindInstrument(
    std::span<const trading::InstrumentSpec> instruments,
    model::InstrumentId id) noexcept {
  for (const auto& instrument : instruments) {
    if (instrument.id == id) {
      return &instrument;
    }
  }
  return nullptr;
}

bool ValidAccount(
    const trading::AccountView& account,
    std::span<const trading::InstrumentSpec> instruments) noexcept {
  if (account.position_count > account.positions.size() ||
      account.cash.balance < 0 || account.cash.reserved < 0 ||
      account.cash.reserved > account.cash.balance ||
      account.cash.available != account.cash.balance - account.cash.reserved) {
    return false;
  }
  for (size_t index = 0; index < account.position_count; ++index) {
    const auto& position = account.positions[index];
    if (FindInstrument(instruments, position.instrument_id) == nullptr ||
        position.total_qty < 0 || position.available_qty < 0 ||
        position.reserved_qty < 0 ||
        position.available_qty > position.total_qty ||
        position.reserved_qty > position.total_qty - position.available_qty) {
      return false;
    }
    for (size_t previous = 0; previous < index; ++previous) {
      if (account.positions[previous].instrument_id == position.instrument_id) {
        return false;
      }
    }
  }
  return true;
}

// Checks exact micro-yuan representation before any product can overflow.
bool FitsNotional(int64_t price, int64_t quantity, uint8_t decimals) noexcept {
  if (price <= 0 || quantity <= 0 || decimals > model::kMaxPriceDecimals) {
    return false;
  }
  int64_t scale = 1;
  for (; decimals < model::kMaxPriceDecimals; ++decimals) {
    scale *= 10;
  }
  const auto maximum = std::numeric_limits<int64_t>::max();
  return price <= maximum / quantity && price * quantity <= maximum / scale;
}

bool ValidOrder(const BrokerRecoveryOrder& order,
                std::span<const trading::InstrumentSpec> instruments,
                uint64_t cut) noexcept {
  const auto& request = order.request;
  const auto* instrument = FindInstrument(instruments, request.instrument_id);
  if (instrument == nullptr || !ValidSide(request.side) ||
      request.position_effect != model::PositionEffect::kNone ||
      request.limit_price <= 0 || request.quantity <= 0 ||
      order.state > model::OrderState::kRejected ||
      order.cumulative_filled_qty < 0 ||
      order.cumulative_filled_qty > request.quantity ||
      order.fact_sequence == 0 || order.fact_sequence > cut ||
      (order.state != model::OrderState::kRejected &&
       order.broker_order_id.empty())) {
    return false;
  }
  if (!FitsNotional(request.limit_price, request.quantity,
                    instrument->price_decimals)) {
    return false;
  }
  switch (order.state) {
    case model::OrderState::kPendingNew:
    case model::OrderState::kRejected:
      return order.cumulative_filled_qty == 0;
    case model::OrderState::kWorking:
    case model::OrderState::kCanceled:
      return order.cumulative_filled_qty < request.quantity;
    case model::OrderState::kFilled:
      return order.cumulative_filled_qty == request.quantity;
  }
  return false;
}

bool SameOrder(const BrokerRecoveryOrder& left,
               const BrokerRecoveryOrder& right) noexcept {
  return left.request_key == right.request_key &&
         left.broker_order_id == right.broker_order_id &&
         left.request.instrument_id == right.request.instrument_id &&
         left.request.side == right.request.side &&
         left.request.limit_price == right.request.limit_price &&
         left.request.quantity == right.request.quantity &&
         left.request.position_effect == right.request.position_effect &&
         left.state == right.state &&
         left.cumulative_filled_qty == right.cumulative_filled_qty &&
         left.fact_sequence == right.fact_sequence;
}

bool SameFill(const BrokerRecoveryFill& left,
              const BrokerRecoveryFill& right) noexcept {
  return left.request_key == right.request_key &&
         left.broker_order_id == right.broker_order_id &&
         left.execution_id == right.execution_id &&
         left.instrument_id == right.instrument_id && left.side == right.side &&
         left.price == right.price && left.quantity == right.quantity &&
         left.event_time_ns == right.event_time_ns &&
         left.has_event_time == right.has_event_time &&
         left.fact_sequence == right.fact_sequence &&
         left.position_effect == right.position_effect;
}

bool ValidFill(const BrokerRecoveryFill& fill, const BrokerRecoveryOrder& order,
               std::span<const trading::InstrumentSpec> instruments) noexcept {
  const auto* instrument = FindInstrument(instruments, fill.instrument_id);
  return instrument != nullptr &&
         fill.position_effect == model::PositionEffect::kNone &&
         FitsNotional(fill.price, fill.quantity, instrument->price_decimals) &&
         !fill.execution_id.empty() && !fill.broker_order_id.empty() &&
         fill.broker_order_id == order.broker_order_id &&
         fill.instrument_id == order.request.instrument_id &&
         fill.side == order.request.side && fill.price > 0 &&
         fill.quantity > 0 && fill.quantity <= order.request.quantity &&
         (fill.has_event_time || fill.event_time_ns == 0) &&
         fill.fact_sequence != 0 && fill.fact_sequence <= order.fact_sequence &&
         (fill.side != model::OrderSide::kBuy ||
          fill.price <= order.request.limit_price) &&
         (fill.side != model::OrderSide::kSell ||
          fill.price >= order.request.limit_price);
}

// References source rows during validation. Only compact indexes/sums are
// staged; the large caller output is untouched until all checks succeed.
struct Workspace {
  std::array<uint16_t, kOrderBuckets> order_lookup = {};
  std::array<uint16_t, kOrderBuckets> broker_lookup = {};
  std::array<uint16_t, kFillBuckets> fill_lookup = {};
  std::array<uint32_t, kMaxRecoveredBrokerOrders> order_rows;
  std::array<uint32_t, kMaxRecoveredBrokerFills> fill_rows;
  std::array<uint16_t, kMaxRecoveredBrokerFills> fill_orders;
  std::array<uint16_t, kMaxRecoveredBrokerOrders> owners;
  std::array<ExternalRequestKey, kMaxRecoveredBrokerOrders> keys;
  std::array<int64_t, kMaxRecoveredBrokerOrders> quantities = {};
  size_t order_count = 0;
  size_t fill_count = 0;

  size_t OrderBucket(std::string_view key,
                     const BrokerRecoveryView& view) const noexcept {
    auto bucket = Hash(key) & (kOrderBuckets - 1);
    while (
        order_lookup[bucket] != 0 &&
        view.orders[order_rows[order_lookup[bucket] - 1]].request_key.view() !=
            key) {
      bucket = (bucket + 1) & (kOrderBuckets - 1);
    }
    return bucket;
  }
};
static_assert(sizeof(Workspace) < 160 * 1024);

}  // namespace

base::Status EncodeExternalRequestKey(
    uint64_t namespace_id, ExternalRequestKind kind, uint64_t sequence,
    char (&output)[kExternalRequestKeyBytes + 1]) noexcept {
  if (namespace_id == 0 || sequence == 0 || !ValidKind(kind)) {
    return {base::ErrorCode::kInvalidArgument, {}};
  }
  std::array<unsigned char, kRequestIdentityBytes> bytes{};
  for (unsigned index = 0; index < 8; ++index) {
    bytes[index] = static_cast<unsigned char>(namespace_id >> (56 - 8 * index));
    bytes[9 + index] = static_cast<unsigned char>(sequence >> (56 - 8 * index));
  }
  bytes[8] = static_cast<unsigned char>(kind);
  char result[kExternalRequestKeyBytes + 1];
  uint32_t accumulator = 0;
  unsigned bits = 0;
  size_t size = 0;
  for (const auto byte : bytes) {
    accumulator = (accumulator << 8) | byte;
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      result[size++] = kBase32Alphabet[(accumulator >> bits) & 31];
    }
  }
  if (bits != 0) {
    result[size++] = kBase32Alphabet[(accumulator << (5 - bits)) & 31];
  }
  result[size] = '\0';
  std::memcpy(output, result, sizeof(result));
  return {};
}

base::Status DecodeExternalRequestKey(std::string_view input,
                                      ExternalRequestKey& output) noexcept {
  if (input.size() != kExternalRequestKeyBytes) {
    return {base::ErrorCode::kCorrupt, {}};
  }
  std::array<unsigned char, kRequestIdentityBytes> bytes{};
  uint32_t accumulator = 0;
  unsigned bits = 0;
  size_t size = 0;
  for (const auto character : input) {
    unsigned value;
    if (character >= 'A' && character <= 'Z') {
      value = static_cast<unsigned>(character - 'A');
    } else if (character >= '2' && character <= '7') {
      value = static_cast<unsigned>(character - '2') + 26;
    } else {
      return {base::ErrorCode::kCorrupt, {}};
    }
    accumulator = (accumulator << 5) | value;
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      bytes[size++] = static_cast<unsigned char>(accumulator >> bits);
    }
  }
  if (size != bytes.size() || (accumulator & ((1U << bits) - 1)) != 0) {
    return {base::ErrorCode::kCorrupt, {}};
  }
  ExternalRequestKey key;
  for (unsigned index = 0; index < 8; ++index) {
    key.namespace_id = (key.namespace_id << 8) | bytes[index];
    key.sequence = (key.sequence << 8) | bytes[9 + index];
  }
  key.kind = static_cast<ExternalRequestKind>(bytes[8]);
  if (key.namespace_id == 0 || key.sequence == 0 || !ValidKind(key.kind)) {
    return {base::ErrorCode::kCorrupt, {}};
  }
  output = key;
  return {};
}

base::Status BuildRecoveredBrokerState(
    const BrokerRecoveryView& view,
    const BrokerRecoveryRequirements& requirements,
    std::span<const DurableStrategyIdentity> identities,
    std::span<const trading::InstrumentSpec> instruments,
    RecoveredBrokerState& output, BrokerRecoveryError* error) noexcept {
  if (error) {
    *error = BrokerRecoveryError::kNone;
  }
  const auto fail = [&](BrokerRecoveryError reason,
                        base::ErrorCode code = base::ErrorCode::kCorrupt) {
    if (error) {
      *error = reason;
    }
    return base::Status{code, {}};
  };
  if (requirements.account_key.empty() ||
      requirements.account_key.size() > kMaxAccountKeyBytes ||
      requirements.account_key.find('\0') != std::string_view::npos ||
      instruments.empty() || instruments.size() > trading::kMaxInstruments) {
    return fail(BrokerRecoveryError::kInvalidRequirements,
                base::ErrorCode::kInvalidArgument);
  }
  if (requirements.account_key != view.account_key) {
    return fail(BrokerRecoveryError::kAccountMismatch,
                base::ErrorCode::kInvalidState);
  }
  if (!view.old_sender_fenced) {
    return fail(BrokerRecoveryError::kSenderNotFenced,
                base::ErrorCode::kInvalidState);
  }
  if (!view.history_complete || !view.terminal_history_complete ||
      !view.account_at_cut || view.consistent_cut == 0 ||
      view.history_lower_bound > requirements.history_lower_bound ||
      view.consistent_cut < requirements.history_lower_bound) {
    return fail(BrokerRecoveryError::kIncompleteHistory,
                base::ErrorCode::kInvalidState);
  }
  if (identities.size() > kMaxRecoveryIdentities ||
      view.orders.size() > kMaxRecoveryOrderFacts ||
      view.fills.size() > kMaxRecoveryFillFacts) {
    return fail(BrokerRecoveryError::kCapacityExceeded,
                base::ErrorCode::kResourceExhausted);
  }
  for (size_t index = 0; index < instruments.size(); ++index) {
    const auto& instrument = instruments[index];
    if (!ValidInstrument(instrument.id) ||
        instrument.price_decimals > model::kMaxPriceDecimals) {
      return fail(BrokerRecoveryError::kInvalidRequirements,
                  base::ErrorCode::kInvalidArgument);
    }
    for (size_t previous = 0; previous < index; ++previous) {
      if (instruments[previous].id == instrument.id) {
        return fail(BrokerRecoveryError::kInvalidRequirements,
                    base::ErrorCode::kInvalidArgument);
      }
    }
  }
  for (size_t index = 0; index < identities.size(); ++index) {
    if (identities[index].namespace_id == 0 ||
        identities[index].client_id == 0) {
      return fail(BrokerRecoveryError::kInvalidIdentity);
    }
    for (size_t previous = 0; previous < index; ++previous) {
      if (identities[previous].namespace_id == identities[index].namespace_id) {
        return fail(BrokerRecoveryError::kInvalidIdentity);
      }
    }
  }
  if (!ValidAccount(view.account, instruments)) {
    return fail(BrokerRecoveryError::kInvalidFact);
  }

  Workspace work;
  for (size_t row = 0; row < view.orders.size(); ++row) {
    const auto& order = view.orders[row];
    ExternalRequestKey key;
    if (!DecodeExternalRequestKey(order.request_key.view(), key).ok() ||
        key.kind != ExternalRequestKind::kNewOrder) {
      return fail(BrokerRecoveryError::kInvalidIdentity);
    }
    if (!ValidOrder(order, instruments, view.consistent_cut)) {
      return fail(BrokerRecoveryError::kInvalidFact);
    }
    size_t owner = 0;
    while (owner < identities.size() &&
           identities[owner].namespace_id != key.namespace_id) {
      ++owner;
    }
    if (owner == identities.size()) {
      return fail(BrokerRecoveryError::kUnknownOwner,
                  base::ErrorCode::kInvalidState);
    }
    const auto bucket = work.OrderBucket(order.request_key.view(), view);
    const auto slot = work.order_lookup[bucket];
    if (slot != 0) {
      if (!SameOrder(view.orders[work.order_rows[slot - 1]], order)) {
        return fail(BrokerRecoveryError::kConflictingOrder);
      }
      continue;
    }
    if (work.order_count == kMaxRecoveredBrokerOrders) {
      return fail(BrokerRecoveryError::kCapacityExceeded,
                  base::ErrorCode::kResourceExhausted);
    }
    if (!order.broker_order_id.empty()) {
      auto broker_bucket =
          Hash(order.broker_order_id.view()) & (kOrderBuckets - 1);
      while (work.broker_lookup[broker_bucket] != 0) {
        const auto other = work.broker_lookup[broker_bucket] - 1;
        if (view.orders[work.order_rows[other]].broker_order_id ==
            order.broker_order_id) {
          return fail(BrokerRecoveryError::kConflictingOrder);
        }
        broker_bucket = (broker_bucket + 1) & (kOrderBuckets - 1);
      }
      work.broker_lookup[broker_bucket] =
          static_cast<uint16_t>(work.order_count + 1);
    }
    work.order_rows[work.order_count] = static_cast<uint32_t>(row);
    work.keys[work.order_count] = key;
    work.owners[work.order_count] = static_cast<uint16_t>(owner);
    work.order_lookup[bucket] = static_cast<uint16_t>(++work.order_count);
  }
  for (size_t row = 0; row < view.fills.size(); ++row) {
    const auto& fill = view.fills[row];
    const auto order_bucket = work.OrderBucket(fill.request_key.view(), view);
    const auto order_slot = work.order_lookup[order_bucket];
    if (order_slot == 0) {
      return fail(BrokerRecoveryError::kInvalidFact);
    }
    const auto order_index = static_cast<size_t>(order_slot - 1);
    const auto& order = view.orders[work.order_rows[order_index]];
    if (!ValidFill(fill, order, instruments)) {
      return fail(BrokerRecoveryError::kInvalidFact);
    }
    auto bucket =
        Hash(fill.execution_id.view(), Hash(fill.request_key.view())) &
        (kFillBuckets - 1);
    bool duplicate = false;
    while (work.fill_lookup[bucket] != 0) {
      const auto& other =
          view.fills[work.fill_rows[work.fill_lookup[bucket] - 1]];
      if (other.request_key == fill.request_key &&
          other.execution_id == fill.execution_id) {
        if (!SameFill(other, fill)) {
          return fail(BrokerRecoveryError::kConflictingFill);
        }
        duplicate = true;
        break;
      }
      bucket = (bucket + 1) & (kFillBuckets - 1);
    }
    if (duplicate) {
      continue;
    }
    if (work.fill_count == kMaxRecoveredBrokerFills) {
      return fail(BrokerRecoveryError::kCapacityExceeded,
                  base::ErrorCode::kResourceExhausted);
    }
    if (fill.quantity > order.request.quantity - work.quantities[order_index]) {
      return fail(BrokerRecoveryError::kInvalidFact);
    }
    work.quantities[order_index] += fill.quantity;
    work.fill_rows[work.fill_count] = static_cast<uint32_t>(row);
    work.fill_orders[work.fill_count] = static_cast<uint16_t>(order_index);
    work.fill_lookup[bucket] = static_cast<uint16_t>(++work.fill_count);
  }
  for (size_t index = 0; index < work.order_count; ++index) {
    if (work.quantities[index] !=
        view.orders[work.order_rows[index]].cumulative_filled_qty) {
      return fail(BrokerRecoveryError::kIncompleteHistory,
                  base::ErrorCode::kInvalidState);
    }
  }

  // A complete broker account already includes every historical execution.
  // Install it once and retain executions solely as identity/dedup evidence.
  output.account = view.account;
  output.history_lower_bound = view.history_lower_bound;
  output.consistent_cut = view.consistent_cut;
  output.order_count = work.order_count;
  output.fill_count = work.fill_count;
  for (size_t index = 0; index < work.order_count; ++index) {
    auto& order = output.orders[index];
    order.identity = work.keys[index];
    order.client_id = identities[work.owners[index]].client_id;
    order.fact = view.orders[work.order_rows[index]];
  }
  for (size_t index = 0; index < work.fill_count; ++index) {
    output.fills[index].order_index = work.fill_orders[index];
    output.fills[index].fact = view.fills[work.fill_rows[index]];
  }
  return {};
}

}  // namespace yquant::tradingd
