#ifndef YQUANT_TESTS_QUOTE_SOURCE_STUB_H_
#define YQUANT_TESTS_QUOTE_SOURCE_STUB_H_

#include "yquant/market/quote_source.h"
#include "yquant/model/quote.h"

namespace yquant::test {

// Supplies one quote set by the test, then returns false until SetQuote is
// called again.
// Records the output address so tests can verify delivery without another copy.
class QuoteSourceStub final : public market::QuoteSource {
 public:
  QuoteSourceStub() = default;
  QuoteSourceStub(const QuoteSourceStub&) = default;
  QuoteSourceStub& operator=(const QuoteSourceStub&) = default;

  // Replaces any pending quote with a copy of quote.
  void SetQuote(const model::Quote& quote) {
    next_quote_ = quote;
    available_ = true;
  }

  // market::QuoteSource implementation.
  // Consumes the pending quote, or returns false without changing quote.
  bool TryRead(model::Quote& quote) override {
    if (!available_) {
      return false;
    }
    quote = next_quote_;
    available_ = false;
    last_output = &quote;
    return true;
  }

  const model::Quote* last_output = nullptr;

 private:
  model::Quote next_quote_;
  bool available_ = false;
};

}  // namespace yquant::test

#endif  // YQUANT_TESTS_QUOTE_SOURCE_STUB_H_
