#ifndef YQUANT_TRADINGD_SERVER_H_
#define YQUANT_TRADINGD_SERVER_H_

#include <memory>

#include "tradingd/config.h"
#include "yquant/base/monotonic_clock.h"

namespace yquant::tradingd {

// Single-thread shared order gateway. The clock outlives this object. Init
// allocates bounded storage; accepting a connection allocates its bounded
// buffers. Established order processing does not allocate or wait for WAL
// sync; mapping stores can still incur kernel page/dirty-pressure delays. Cold
// client registration persists identity before Welcome. Persistent identities
// remain retained across restarts, including after EOF.
class Server {
 public:
  // Borrows a non-null clock that must outlive this server.
  explicit Server(const base::MonotonicClock* clock) noexcept;
  ~Server() noexcept;
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  // Initializes a new account or recovers configured persistent state before
  // opening the listener. Failure releases partial resources.
  [[nodiscard]] base::Status Init(const ServerConfig& config) noexcept;
  // Services bounded client requests, broker reports and automatic cancels.
  // Failure is terminal; retained account() still reflects committed events.
  [[nodiscard]] base::Status Service(std::uint64_t now) noexcept;
  // Stops new connections/orders and cancels every outstanding order. Success
  // requires is_drained(); shutdown timeout returns an error from Service.
  void BeginStopping(std::uint64_t now) noexcept;
  bool is_drained() const noexcept;
  std::uint16_t port() const noexcept;
  const trading::AccountView& account() const noexcept;
  const char* error_detail() const noexcept { return error_detail_; }
  // Writes retained nonterminal orders and unresolved cancels to stderr. This
  // cold diagnostic does not change account facts or confirm cancellation.
  void ReportUnconfirmed() const noexcept;
  // Resource release alone does not imply confirmed broker cancellation.
  void Close() noexcept;
  // Cold orderly shutdown: requires drained orders, waits for a final checked
  // checkpoint and background sync. Failure retains diagnostics and state
  // files.
  [[nodiscard]] base::Status Finish() noexcept;

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
  [[nodiscard]] base::Status Handle(std::size_t index,
                                    const trading::Message& message,
                                    std::uint64_t now) noexcept;
  // Authenticates and persists a new client before sending its account view.
  base::Status HandleHello(std::size_t index,
                           const trading::Message& message) noexcept;
  // Validates the simulation publisher and updates quote-driven valuations.
  base::Status HandleSimulationQuote(std::size_t index,
                                     const trading::Message& message,
                                     std::uint64_t now) noexcept;
  // Submits a client-local order or persists and sends its rejection.
  base::Status HandleNewOrder(std::size_t index,
                              const trading::Message& message,
                              std::uint64_t now) noexcept;
  // Maps a client cancel to its core order or sends an explicit rejection.
  base::Status HandleCancelOrder(std::size_t index,
                                 const trading::Message& message) noexcept;
  // Accepts available client slots without reusing retained identities.
  base::Status AcceptClients(std::uint64_t now) noexcept;
  // Reads a bounded request batch and sends due heartbeats for one client.
  base::Status ServiceClient(std::size_t index, std::uint64_t now) noexcept;
  // Flushes queued messages and disconnects clients past their stop deadline.
  void FlushClients(std::uint64_t now) noexcept;
  // Commits a bounded broker batch, then routes reports and committed views.
  [[nodiscard]] base::Status Dispatch(std::uint64_t now) noexcept;
  // Scans a bounded round-robin batch; existing user cancels finish first.
  void ScheduleClientCancels(std::uint64_t now) noexcept;
  // Queues Drained once all client-owned orders and cancels are terminal.
  void NotifyDrained() noexcept;
  const base::MonotonicClock& clock_;
  std::unique_ptr<Storage> storage_;
  char error_detail_[768] = {};
};

}  // namespace yquant::tradingd
#endif  // YQUANT_TRADINGD_SERVER_H_
