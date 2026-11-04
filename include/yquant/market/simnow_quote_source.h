#ifndef YQUANT_MARKET_SIMNOW_QUOTE_SOURCE_H_
#define YQUANT_MARKET_SIMNOW_QUOTE_SOURCE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "yquant/market/quote_source.h"

namespace yquant::market {

// Maps an exact CTP contract code to the application's instrument identity.
struct SimNowSubscription {
  std::string symbol;
  std::uint32_t instrument_id = 0;
};

// A subscription group on one front. The single-contract symbol/instrument_id
// pair remains supported. When subscriptions is nonempty, leave that pair empty.
// Connect copies all strings before returning; supply a matching market SDK.
struct SimNowMarketConfig {
  std::string_view front, symbol, broker, user, password;
  std::uint32_t instrument_id = 0;
  // An explicit anonymous login sends empty broker, user and password fields.
  bool anonymous = false;
  std::vector<SimNowSubscription> subscriptions{};
};

// Receives a group of SimNow market subscriptions. Call Connect, Read and destroy it
// on the business thread. Read advances login and subscription without waiting
// for a reply. A failed session returns idle and must be destroyed to restart.
class SimNowQuoteSource : public QuoteSource {
 public:
  // Starts the SDK connection. Invalid parameters or API creation failure
  // return nullptr; asynchronous errors are reported through failed().
  static std::unique_ptr<SimNowQuoteSource> Connect(const SimNowMarketConfig&);
  virtual bool failed() const = 0;
  // True only after all requested contracts have successful acknowledgments.
  // A failed/disconnected session is never ready, even with unread quotes.
  virtual bool ready() const = 0;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_SIMNOW_QUOTE_SOURCE_H_
