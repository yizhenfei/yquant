#ifndef YQUANT_EXAMPLES_MARKET_FIXED_QUOTE_SOURCE_H_
#define YQUANT_EXAMPLES_MARKET_FIXED_QUOTE_SOURCE_H_

#include <cstddef>
#include <span>

#include "yquant/model/quote.h"

namespace yquant::examples {

// Reads quotes from a fixed input sequence for validation.
// Does not own the input data. Keep that data alive and unchanged while using
// this source. Next copies quotes into the caller's output buffer.
class FixedQuoteSource {
 public:
  explicit FixedQuoteSource(std::span<const model::Quote> quotes)
      : quotes_(quotes) {}
  FixedQuoteSource(const FixedQuoteSource&) = delete;
  FixedQuoteSource& operator=(const FixedQuoteSource&) = delete;

  // Copies the next quote into `quote` and returns true.
  // Returns false when all quotes have been read, without changing `quote`.
  bool Next(model::Quote& quote) {
    if (exhausted()) {
      return false;
    }
    quote = quotes_[next_index_++];
    return true;
  }

  bool exhausted() const { return next_index_ == quotes_.size(); }

 private:
  std::span<const model::Quote> quotes_;
  std::size_t next_index_ = 0;
};

}  // namespace yquant::examples

#endif  // YQUANT_EXAMPLES_MARKET_FIXED_QUOTE_SOURCE_H_
