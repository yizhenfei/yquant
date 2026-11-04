#ifndef YQUANT_MARKET_MARKET_SERVICE_H_
#define YQUANT_MARKET_MARKET_SERVICE_H_

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "yquant/container/service.h"
#include "yquant/market/quote_source.h"
#include "yquant/model/quote.h"

namespace yquant::market {

// Receives subscribed quote updates. Implementations copy any data needed after
// the callback returns; the producer retains ownership of the input.
class QuoteListener {
 public:
  virtual ~QuoteListener() = default;
  // The quote is borrowed until OnQuote returns.
  virtual void OnQuote(const model::Quote& quote) = 0;
};

// Reads quotes from a source and delivers each quote to listeners subscribed
// to its instrument. Use this service on one business thread.
// The source must outlive this service.
// Change subscriptions only between Run calls, never from OnQuote callbacks.
// Keep each listener alive until all its subscriptions have been removed.
class MarketService final : public container::Service {
 public:
  explicit MarketService(QuoteSource& source) : source_(source) {}
  MarketService(const MarketService&) = delete;
  MarketService& operator=(const MarketService&) = delete;

  // Subscribes listener to instrument_id.
  // Repeating the same instrument_id and listener pair has no effect.
  void Subscribe(std::uint32_t instrument_id, QuoteListener& listener);
  // Removes listener's subscription to instrument_id if one exists.
  void Unsubscribe(std::uint32_t instrument_id, QuoteListener& listener);

  // container::Service implementation.
  // Reads and dispatches quotes on the calling thread.
  // Stops when TryRead returns false or the per-call read limit is reached.
  // Calling Run again before the current call returns does nothing.
  void Run() override;

 private:
  // Delivers quote only to listeners subscribed to its instrument.
  void Dispatch(const model::Quote& quote);

  QuoteSource& source_;
  bool running_ = false;
  std::unordered_map<std::uint32_t, std::vector<QuoteListener*>> subscriptions_;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_MARKET_SERVICE_H_
