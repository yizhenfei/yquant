#include "yquant/market/local_market_client.h"

#include <cstdlib>

namespace yquant::market {

LocalMarketClient::~LocalMarketClient() {
  if (!Close()) std::abort();
}

bool LocalMarketClient::BindListener(QuoteListener& listener) {
  if (closed_ || listener_) return false;
  listener_ = &listener;
  return true;
}

bool LocalMarketClient::Subscribe(std::uint32_t instrument_id) {
  if (!instrument_id || !listener_ || !service_.CanDetachListeners()) return false;
  service_.Subscribe(instrument_id, *this);
  registered_ = true;
  return true;
}

bool LocalMarketClient::Unsubscribe(std::uint32_t instrument_id) {
  if (!listener_ || !service_.CanDetachListeners()) return false;
  service_.Unsubscribe(instrument_id, *this);
  return true;
}

bool LocalMarketClient::CanClose() const {
  return closed_ || !registered_ || service_.CanDetachListeners();
}

bool LocalMarketClient::Close() {
  if (closed_) return true;
  if (registered_ && !service_.DetachListener(*this)) return false;
  closed_ = true;
  listener_ = nullptr;
  return true;
}

void LocalMarketClient::OnQuote(const model::Quote& quote) {
  if (listener_) listener_->OnQuote(quote);
}

}  // namespace yquant::market
