#include "simnow_submissions.h"

#include <algorithm>
#include <charconv>
#include <cmath>

#include "simnow_fields.h"
#include "simnow_order_tag.h"

namespace yquant::trading::internal {
namespace {

constexpr std::uint64_t kMaxRef = 999999999999ULL;
static_assert(sizeof(TThostFtdcOrderRefType) == 13);

std::optional<std::uint64_t> Reference(std::string_view text) {
  const auto first = text.find_first_not_of(' ');
  if (first == text.npos) return std::nullopt;
  text = text.substr(first, text.find_last_not_of(' ') - first + 1);
  std::uint64_t value = 0;
  const auto end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (ec != std::errc{} || ptr != end || value > kMaxRef) return std::nullopt;
  return value;
}

char NativeOffset(Offset offset) {
  switch (offset) {
    case Offset::kOpen: return THOST_FTDC_OF_Open;
    case Offset::kClose: return THOST_FTDC_OF_Close;
    case Offset::kCloseToday: return THOST_FTDC_OF_CloseToday;
    case Offset::kCloseYesterday: return THOST_FTDC_OF_CloseYesterday;
  }
  return 0;
}

char NativeHedge(HedgeFlag hedge) {
  switch (hedge) {
    case HedgeFlag::kSpeculation: return THOST_FTDC_HF_Speculation;
    case HedgeFlag::kArbitrage: return THOST_FTDC_HF_Arbitrage;
    case HedgeFlag::kHedge: return THOST_FTDC_HF_Hedge;
  }
  return 0;
}

}  // namespace

SimNowSubmissions::SimNowSubmissions(const SimNowInstruments& instruments) {
  for (const auto& [contract, binding] : instruments) {
    instruments_.emplace(binding.instrument_id, contract);
  }
  orders_.reserve(256);
  requests_.reserve(256);
  refs_.reserve(256);
}

void SimNowSubmissions::Start(int front, int session, std::string_view max_ref,
                              const SimNowOrderHistory& history) {
  orders_.clear();
  requests_.clear();
  refs_.clear();
  exchanges_.clear();
  front_ = front;
  session_ = session;
  last_ref_ = max_ref.find_first_not_of(' ') == max_ref.npos
                  ? std::optional<std::uint64_t>(0) : Reference(max_ref);
  if (!last_ref_) return;
  for (const auto& o : history.orders()) {
    if (o.front != front || o.session != session) continue;
    if (const auto ref = Reference(o.ref)) *last_ref_ = std::max(*ref, *last_ref_);
  }
}

bool SimNowSubmissions::Prepare(const OrderInfo& o, int request_id,
                                CThostFtdcInputOrderField& r) {
  const auto instrument = instruments_.find(o.instrument_id);
  if (request_id <= 0 || requests_.contains(request_id) ||
      instrument == instruments_.end() || !last_ref_ || *last_ref_ >= kMaxRef ||
      !std::isfinite(o.price) || o.price <= 0 || o.quantity <= 0 ||
      (o.side != Side::kBuy && o.side != Side::kSell)) return false;
  r.CombOffsetFlag[0] = NativeOffset(o.offset);
  r.CombHedgeFlag[0] = NativeHedge(o.hedge);
  if (!r.CombOffsetFlag[0] || !r.CombHedgeFlag[0] ||
      !Copy(r.ExchangeID, instrument->second.first) ||
      !Copy(r.InstrumentID, instrument->second.second) ||
      !WriteOrderTag({o.order_id, o.strategy_id}, r)) return false;
  const auto ref = *last_ref_ + 1;
  auto [it, added] = orders_.try_emplace(o.order_id, Entry{o, request_id, ref, {}});
  if (!added) return false;
  *last_ref_ = ref;
  std::to_chars(r.OrderRef, r.OrderRef + sizeof(r.OrderRef) - 1, ref);
  r.RequestID = request_id;
  r.Direction = o.side == Side::kBuy ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
  r.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
  r.LimitPrice = o.price;
  r.VolumeTotalOriginal = o.quantity;
  r.TimeCondition = THOST_FTDC_TC_GFD;
  r.VolumeCondition = THOST_FTDC_VC_AV;
  r.MinVolume = 1;
  r.ContingentCondition = THOST_FTDC_CC_Immediately;
  r.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
  requests_.emplace(request_id, &it->second);
  refs_.emplace(ref, &it->second);
  return true;
}

const SimNowSubmissions::Entry* SimNowSubmissions::ByRequest(int id) const {
  const auto it = requests_.find(id);
  return it == requests_.end() ? nullptr : it->second;
}

const SimNowSubmissions::Entry* SimNowSubmissions::ByReference(std::string_view text) const {
  const auto ref = Reference(text);
  if (!ref) return nullptr;
  const auto it = refs_.find(*ref);
  return it == refs_.end() ? nullptr : it->second;
}

const SimNowSubmissions::Entry* SimNowSubmissions::ByExchange(
    const std::string& exchange, const std::string& system_id) const {
  const auto scope = exchanges_.find(exchange);
  if (scope == exchanges_.end()) return nullptr;
  const auto found = scope->second.find(system_id);
  return found == scope->second.end() ? nullptr : found->second;
}

bool SimNowSubmissions::MatchesTrade(const Entry& e, char direction, char offset, char hedge) const {
  return direction == (e.order.side == Side::kBuy ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell) &&
         offset == NativeOffset(e.order.offset) && hedge == NativeHedge(e.order.hedge);
}

bool SimNowSubmissions::MatchesInstrument(const Entry& e, std::string_view exchange,
                                          std::string_view symbol) const {
  const auto& contract = instruments_.at(e.order.instrument_id);
  return (exchange.empty() || contract.first == exchange) && contract.second == symbol;
}

const SimNowSubmissions::Entry* SimNowSubmissions::Match(
    const SimNowOrder& u, bool recovered_system_id, bool& conflict) {
  conflict = false;
  const auto* local = u.front == front_ && u.session == session_ ? ByReference(u.ref) : nullptr;
  const auto* native = ByExchange(u.exchange, u.system_id);
  const auto* e = local ? local : native;
  if (!e) return nullptr;
  const auto& o = e->order;
  const bool has_session = u.front != 0 || u.session != 0;
  if (recovered_system_id || (!u.system_id.empty() && u.exchange.empty()) ||
      (native && has_session && (u.front != front_ || u.session != session_)) ||
      (native && !u.ref.empty() && Reference(u.ref) != native->ref) ||
      !MatchesInstrument(*e, u.exchange, u.symbol) ||
      (!e->system_id.empty() && !u.system_id.empty() && e->system_id != u.system_id) ||
      (u.owner && (u.owner->order_id != o.order_id || u.owner->strategy_id != o.strategy_id)) ||
      u.quantity != o.quantity || u.price != o.price || !MatchesTrade(*e, u.direction, u.offset, u.hedge)) {
    conflict = true;
    return nullptr;
  }
  if (!u.system_id.empty() && !native) {
    auto& entry = orders_.at(o.order_id);
    entry.system_id = u.system_id;
    exchanges_[u.exchange].emplace(u.system_id, &entry);
  }
  return e;
}

}  // namespace yquant::trading::internal
