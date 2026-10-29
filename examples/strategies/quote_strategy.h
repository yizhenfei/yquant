#ifndef YQUANT_EXAMPLES_STRATEGIES_QUOTE_STRATEGY_H_
#define YQUANT_EXAMPLES_STRATEGIES_QUOTE_STRATEGY_H_

#include <cstdint>

#include "yquant/market/market_client.h"
#include "yquant/model/quote.h"

namespace yquant::examples {

// Proposed bid/ask prices emitted by the validation strategy.
struct QuoteIntent {
  std::uint32_t instrument_id = 0;
  double bid_price = 0;
  double ask_price = 0;
  std::uint64_t sequence = 0;
};

// Consumes intents delivered synchronously by the validation strategy.
// Implementors copy any intent needed after the callback returns.
class QuoteIntentListener {
 public:
  virtual ~QuoteIntentListener() = default;
  // The intent is borrowed until OnQuoteIntent returns.
  virtual void OnQuoteIntent(const QuoteIntent& intent) = 0;
};

// Validation strategy that emits one intent at each received bid/ask pair.
// Used by StrategyService; the output listener is borrowed and must outlive
// this strategy.
class QuoteStrategy final : public market::QuoteListener {
 public:
  explicit QuoteStrategy(QuoteIntentListener& listener) : listener_(listener) {}
  QuoteStrategy(const QuoteStrategy&) = delete;
  QuoteStrategy& operator=(const QuoteStrategy&) = delete;

  // market::QuoteListener implementation.
  void OnQuote(const model::Quote& quote) override {
    const QuoteIntent intent{quote.instrument_id, quote.bid_price,
                             quote.ask_price, quote.sequence};
    listener_.OnQuoteIntent(intent);
  }

 private:
  QuoteIntentListener& listener_;
};

}  // namespace yquant::examples

#endif  // YQUANT_EXAMPLES_STRATEGIES_QUOTE_STRATEGY_H_
