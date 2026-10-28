#ifndef YQUANT_EXAMPLES_MARKET_QUOTE_PUBLISHER_H_
#define YQUANT_EXAMPLES_MARKET_QUOTE_PUBLISHER_H_

#include "market/fixed_quote_source.h"
#include "yquant/container/container.h"
#include "yquant/market/market_service.h"
#include "yquant/model/quote.h"

namespace yquant::examples {

// Publishes one fixed-source quote per Poll, reusing its quote buffer.
// The caller must keep the source and market service alive while using this
// publisher. Register it with a container to process the finite input sequence.
class QuotePublisher final : public container::Pollable {
 public:
  QuotePublisher(FixedQuoteSource& source, market::MarketService& market)
      : source_(source), market_(market) {}
  QuotePublisher(const QuotePublisher&) = delete;
  QuotePublisher& operator=(const QuotePublisher&) = delete;

  // Returns kFinished for empty input or after delivering the final quote.
  container::PollResult Poll() override {
    if (!source_.Next(quote_)) {
      return container::PollResult::kFinished;
    }
    market_.Publish(quote_);
    return source_.exhausted() ? container::PollResult::kFinished
                               : container::PollResult::kContinue;
  }

 private:
  FixedQuoteSource& source_;
  market::MarketService& market_;
  model::Quote quote_;
};

}  // namespace yquant::examples

#endif  // YQUANT_EXAMPLES_MARKET_QUOTE_PUBLISHER_H_
