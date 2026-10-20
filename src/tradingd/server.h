#ifndef YQUANT_TRADINGD_SERVER_H_
#define YQUANT_TRADINGD_SERVER_H_

#include <memory>

#include "strategyd/monotonic_clock.h"
#include "tradingd/config.h"

namespace yquant::tradingd {

// Single-thread shared order gateway. The clock outlives this object. Init
// allocates bounded storage; accepting a connection allocates its bounded
// buffers. Established-connection order processing does not allocate or wait.
// A client identity is never reused within a generation, including after EOF.
class Server {
 public:
  explicit Server(const strategyd::MonotonicClock& clock) noexcept;
  ~Server() noexcept;
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  // Prepares a fresh account and listener. Failure releases partial resources.
  [[nodiscard]] base::Status Init(const ServerConfig& config) noexcept;
  // Services bounded client requests, broker reports and automatic cancels.
  // Failure is terminal; retained account() still reflects committed events.
  [[nodiscard]] base::Status Service(std::uint64_t now) noexcept;
  // Stops new connections/orders and cancels every outstanding order. Success
  // requires is_drained(); shutdown timeout returns an error from Service.
  void BeginStopping(std::uint64_t now) noexcept;
  bool is_drained() const noexcept;
  std::uint16_t port() const noexcept;
  const AccountView& account() const noexcept;
  const char* error_detail() const noexcept { return error_detail_; }
  // Writes retained nonterminal orders and unresolved cancels to stderr. This
  // cold diagnostic does not change account facts or confirm cancellation.
  void ReportUnconfirmed() const noexcept;
  // Resource release alone does not imply confirmed broker cancellation.
  void Close() noexcept;

 private:
  struct Storage;
  // Retains a diagnostic without changing the committed account.
  [[nodiscard]] base::Status Fail(base::ErrorCode code,
                                  const char* detail) noexcept;
  // Retains authenticated identity/ownership after socket closure. Subsequent
  // service cancels working orders and waits for confirmation to release funds.
  void Disconnect(std::size_t index) noexcept;
  // Copies committed cash/positions while retaining the current revision.
  void RefreshAccount() noexcept;
  // Queues the account to connected peers; full/failed peers disconnect alone.
  void BroadcastAccount() noexcept;
  // Protocol violations disconnect the requester; business rejections emit an
  // explicit OrderUpdate. Input is borrowed only during this call.
  [[nodiscard]] base::Status Handle(std::size_t index, const Message& message,
                                    std::uint64_t now) noexcept;
  // Commits a bounded broker batch, then routes reports and committed views.
  [[nodiscard]] base::Status Dispatch(std::uint64_t now) noexcept;
  // Scans a bounded round-robin batch; existing user cancels finish first.
  void ScheduleClientCancels(std::uint64_t now) noexcept;
  // Queues Drained once all client-owned orders and cancels are terminal.
  void NotifyDrained() noexcept;
  const strategyd::MonotonicClock& clock_;
  std::unique_ptr<Storage> storage_;
  char error_detail_[768] = {};
};

}  // namespace yquant::tradingd
#endif  // YQUANT_TRADINGD_SERVER_H_
