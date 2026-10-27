#ifndef YQUANT_MODEL_QUOTE_H_
#define YQUANT_MODEL_QUOTE_H_

#include <cstdint>

namespace yquant::model {

// Best bid/ask prices and available quantities for one instrument.
// Copy the value when retaining a snapshot beyond a quote callback.
struct Quote {
  std::uint32_t instrument_id = 0;
  double bid_price = 0;
  double ask_price = 0;
  std::int32_t bid_quantity = 0;
  std::int32_t ask_quantity = 0;
};

}  // namespace yquant::model

#endif  // YQUANT_MODEL_QUOTE_H_
