#include "yquant/market/market_service.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "yquant/market/quote_source.h"
#include "yquant/model/quote.h"

namespace yquant::market {
namespace {

// Bounds each batch so a busy source yields to other services.
constexpr std::size_t kMaxQuotesPerRun = 64;

}  // namespace

void MarketService::Subscribe(std::uint32_t instrument_id,
                              QuoteListener& listener) {
  auto& listeners = subscriptions_[instrument_id];
  if (std::find(listeners.begin(), listeners.end(), &listener) ==
      listeners.end()) {
    listeners.push_back(&listener);
  }
}

void MarketService::Unsubscribe(std::uint32_t instrument_id,
                                QuoteListener& listener) {
  const auto entry = subscriptions_.find(instrument_id);
  if (entry == subscriptions_.end()) {
    return;
  }
  std::erase(entry->second, &listener);
  if (entry->second.empty()) {
    subscriptions_.erase(entry);
  }
}

void MarketService::Run() {
  if (running_) {
    return;
  }
  running_ = true;
  model::Quote quote;
  for (std::size_t count = 0; count < kMaxQuotesPerRun; ++count) {
    if (source_.Read(quote) == QuoteReadResult::kIdle) {
      break;
    }
    Dispatch(quote);
  }
  running_ = false;
}

void MarketService::Dispatch(const model::Quote& quote) {
  const auto entry = subscriptions_.find(quote.instrument_id);
  if (entry == subscriptions_.end()) {
    return;
  }
  for (QuoteListener* listener : entry->second) {
    listener->OnQuote(quote);
  }
}

}  // namespace yquant::market
