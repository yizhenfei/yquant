#ifndef YQUANT_SRC_TRADING_SIMNOW_SUBMISSIONS_H_
#define YQUANT_SRC_TRADING_SIMNOW_SUBMISSIONS_H_

#include <ThostFtdcTraderApi.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "yquant/trading/simnow_startup_snapshot.h"

namespace yquant::trading::internal {

// Tracks limit orders issued in one authenticated session on the business
// thread. Request IDs identify front responses; exchange IDs are bound only
// after the front/session/ref tuple identifies the same submitted order.
class SimNowSubmissions {
 public:
  // Identifies one invocation before and after the exchange assigns system_id.
  struct Entry {
    OrderInfo order;
    int request_id;
    std::uint64_t ref;
    std::string system_id;
  };

  explicit SimNowSubmissions(const SimNowInstruments& instruments);
  SimNowSubmissions(const SimNowSubmissions&) = delete;
  SimNowSubmissions& operator=(const SimNowSubmissions&) = delete;
  // Starts above the login's MaxOrderRef and any larger reference observed in
  // this session's startup snapshot. An invalid or exhausted floor blocks sends.
  void Start(int front, int session, std::string_view max_ref,
             const SimNowOrderHistory& history);
  // Writes the order fields into a zero-initialized request containing the
  // account fields. Installs all associations before the SDK sees the request.
  bool Prepare(const OrderInfo& order, int request_id, CThostFtdcInputOrderField& request);
  const Entry* ByRequest(int request_id) const;
  const Entry* ByReference(std::string_view ref) const;
  const Entry* ByExchange(const std::string& exchange, const std::string& system_id) const;
  // Returns nullptr for an unrelated order. A conflicting native identifier
  // or order parameter sets conflict and leaves the existing binding intact.
  const Entry* Match(const SimNowOrder& update, bool recovered_system_id, bool& conflict);
  bool MatchesTrade(const Entry& entry, char direction, char offset, char hedge) const;
  bool MatchesInstrument(const Entry& entry, std::string_view exchange,
                         std::string_view symbol) const;

 private:
  using Contract = std::pair<std::string, std::string>;
  std::unordered_map<std::uint32_t, Contract> instruments_;
  std::unordered_map<OrderId, Entry, OrderIdHash> orders_;
  std::unordered_map<int, Entry*> requests_;
  std::unordered_map<std::uint64_t, Entry*> refs_;
  std::unordered_map<std::string, std::unordered_map<std::string, Entry*>> exchanges_;
  int front_ = 0, session_ = 0;
  std::optional<std::uint64_t> last_ref_;
};

}  // namespace yquant::trading::internal

#endif  // YQUANT_SRC_TRADING_SIMNOW_SUBMISSIONS_H_
