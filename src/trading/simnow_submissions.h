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

// Matches counter responses to orders submitted in one authenticated session.
// Uses request IDs for front responses and session/reference or exchange IDs
// for order reports. Use on the business thread.
class SimNowSubmissions {
 public:
  // Associates a submitted order with its request and counter identifiers.
  struct Entry {
    OrderInfo order;
    int request_id;
    std::uint64_t ref;
    std::string system_id, wire_system_id;
  };

  explicit SimNowSubmissions(const SimNowInstruments& instruments);
  SimNowSubmissions(const SimNowSubmissions&) = delete;
  SimNowSubmissions& operator=(const SimNowSubmissions&) = delete;
  // Starts above the login's MaxOrderRef and any larger reference observed in
  // this session's startup snapshot. An invalid or exhausted floor blocks sends.
  void Start(int front, int session, std::string_view max_ref,
             const SimNowOrderHistory& history);
  // Fills a zero-initialized request whose account fields are already set.
  // On success registers its identifiers and returns true; the caller can then
  // send it to the SDK. Returns false if the order cannot be prepared.
  bool Prepare(const OrderInfo& order, int request_id, CThostFtdcInputOrderField& request);
  // Fills the target of a delete action for an order sent by this session.
  bool SetCancelTarget(OrderId id, CThostFtdcInputOrderActionField& action) const;
  // These lookups return nullptr if the native identifier is unknown.
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
