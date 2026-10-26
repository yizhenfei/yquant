#ifndef YQUANT_TRADINGD_SERVER_H_
#define YQUANT_TRADINGD_SERVER_H_

// Assembles the trading gateway. Included only by its entry point,
// implementation and tests; reusable libraries do not depend on this header.

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <optional>

#include "tradingd/config.h"
#include "yquant/base/monotonic_clock.h"
#include "yquant/md/instrument_scope.h"
#include "yquant/trading/connection.h"
#include "yquant/trading/ctp_trading_adapter.h"
#include "yquant/trading/mock_trading_adapter.h"
#include "yquant/trading/simulated_futures_exchange.h"
#include "yquant/trading/trading_engine.h"

namespace yquant::tradingd {

// Client-local identities remain retained even after a disconnected socket.
struct LocalOrder {
  model::ClientOrderId core_id;
  model::OrderSnapshot rejected;
  uint64_t report_sequence = 0;
  model::CancelRequestId core_cancel;
  model::CancelRequestId local_cancel;
  uint64_t retry_at = 0;
};

// One retained client identity and its bounded local order namespace. Socket
// timestamps/flags are runtime-only; offline identities still occupy capacity.
struct Client {
  trading::Connection connection;
  uint64_t id = 0;
  bool authenticated = false;
  bool stopping = false;
  bool requested_stop = false;
  bool drained_sent = false;
  uint64_t last_receive = 0;
  uint64_t last_heartbeat = 0;
  uint64_t stop_time = 0;
  std::array<trading::InstrumentSpec, trading::kMaxInstruments> instruments =
      {};
  size_t instrument_count = 0;
  std::array<LocalOrder, trading::kMaxTradingOrders> orders = {};
  size_t order_count = 0;
  uint64_t last_cancel_id = 0;
  // Closes the transport and clears the retained identity and order namespace.
  void Reset() noexcept;
};

// Dense ledger slot to stable core ID and retained client/local ownership.
struct OrderOwner {
  model::ClientOrderId core_id;
  size_t client = 0;
  size_t local = 0;
};

// Single-thread shared order gateway. The clock outlives this object. Construct
// on the heap because the client and order arrays are large. Init prepares
// account storage; accepting a connection allocates its bounded buffers.
// Established order processing does not allocate. Client identities remain
// retained after disconnection until the server closes.
class Server final {
 public:
  // Borrows a non-null clock that must outlive this server.
  explicit Server(const base::MonotonicClock* clock) noexcept;
  ~Server() noexcept;
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  // Initializes the account before opening the listener. Failure releases
  // partial resources.
  [[nodiscard]] base::Status Init(const ServerConfig& config) noexcept;
  // Services bounded client requests, broker reports and automatic cancels.
  // Failure is terminal; retained account() still reflects committed events.
  [[nodiscard]] base::Status Service(uint64_t now) noexcept;
  // Stops new connections/orders and cancels every outstanding order. Success
  // requires is_drained(); shutdown timeout returns an error from Service.
  void BeginStopping(uint64_t now) noexcept;
  bool is_drained() const noexcept;
  uint16_t port() const noexcept;
  const trading::AccountView& account() const noexcept;
  const char* error_detail() const noexcept { return error_detail_; }
  // Writes retained nonterminal orders and unresolved cancels to stderr. This
  // cold diagnostic does not change account facts or confirm cancellation.
  void ReportUnconfirmed() const noexcept;
  // Resource release alone does not imply confirmed broker cancellation.
  void Close() noexcept;

 private:
  // Selects the configured adapter and forwards its operations.
  class Adapter final : public trading::TradingAdapter {
   public:
    // Borrows a non-null server whose lifetime covers this wrapper.
    explicit Adapter(Server* server) noexcept;
    base::Status Prepare(
        const trading::TradingConfig& config) noexcept override;
    base::Status Start(uint64_t now) noexcept override;
    void BeginStopping() noexcept override;
    base::Status ReadAccountSnapshot(
        trading::TradingAccountSnapshot& out) noexcept override;
    base::Status ReadBrokerAccountSnapshot(
        trading::BrokerAccountSnapshot& out) noexcept override;
    base::Status SendOrder(model::ClientOrderId id,
                           const model::OrderRequest& request,
                           uint64_t now) noexcept override;
    base::Status SendCancel(model::ClientOrderId id,
                            const model::AdapterOrderId& adapter_order_id,
                            model::CancelRequestId cancel,
                            uint64_t now) noexcept override;
    trading::TradingPollResult Poll(uint64_t now,
                                    model::TradingEvent& out) noexcept override;
    std::optional<uint64_t> next_deadline() const noexcept override;
    bool is_drained() const noexcept override;
    base::Status Stop() noexcept override;
    const char* error_detail() const noexcept override;

   private:
    Server* server_;
  };

  static constexpr size_t kOwnerLookupCapacity = 2 * trading::kMaxTradingOrders;
  static_assert((kOwnerLookupCapacity & (kOwnerLookupCapacity - 1)) == 0);
  // Checks whether the client has registered this instrument.
  static bool HasInstrument(const Client& client,
                            model::InstrumentId id) noexcept;
  // Retains a diagnostic without changing the committed account.
  [[nodiscard]] base::Status Fail(base::ErrorCode code,
                                  const char* detail) noexcept;
  // Retains authenticated identity/ownership after socket closure. Subsequent
  // service cancels working orders and waits for confirmation to release funds.
  void Disconnect(size_t index) noexcept;
  // Copies committed cash/positions while retaining the current revision.
  void RefreshAccount() noexcept;
  // Queues the account to connected peers; full/failed peers disconnect alone.
  void BroadcastAccount() noexcept;
  // Protocol violations disconnect the requester; business rejections emit an
  // explicit OrderUpdate. Input is borrowed only during this call.
  [[nodiscard]] base::Status Handle(size_t index,
                                    const trading::Message& message,
                                    uint64_t now) noexcept;
  // Authenticates a new client before sending its account view.
  base::Status HandleHello(size_t index,
                           const trading::Message& message) noexcept;
  // Validates the simulation publisher and updates quote-driven valuations.
  base::Status HandleSimulationQuote(size_t index,
                                     const trading::Message& message,
                                     uint64_t now) noexcept;
  // Submits a client-local order or sends its rejection.
  base::Status HandleNewOrder(size_t index, const trading::Message& message,
                              uint64_t now) noexcept;
  // Maps a client cancel to its core order or sends an explicit rejection.
  base::Status HandleCancelOrder(size_t index,
                                 const trading::Message& message) noexcept;
  // Accepts available client slots without reusing retained identities.
  base::Status AcceptClients(uint64_t now) noexcept;
  // Reads a bounded request batch and sends due heartbeats for one client.
  base::Status ServiceClient(size_t index, uint64_t now) noexcept;
  // Flushes queued messages and disconnects clients past their stop deadline.
  void FlushClients(uint64_t now) noexcept;
  // Commits a bounded broker batch, then routes reports and committed views.
  [[nodiscard]] base::Status Dispatch(uint64_t now) noexcept;
  // Scans a bounded round-robin batch; existing user cancels finish first.
  void ScheduleClientCancels(uint64_t now) noexcept;
  // Queues Drained once all client-owned orders and cancels are terminal.
  void NotifyDrained() noexcept;
  // Checks retained orders and unresolved cancels for one client.
  bool ClientDrained(const Client& client) const noexcept;
  // Resets runtime values after transports and account state close.
  void ResetRuntimeState() noexcept;
  // Missing IDs return owners_.size(); lookup is bounded and allocation-free.
  size_t OwnerIndex(model::ClientOrderId id) const noexcept;
  // Publishes one previously unused dense slot in the fixed identity lookup.
  void BindOwner(size_t slot, const OrderOwner& owner) noexcept;

  ServerConfig config_;
  trading::Listener listener_;
  md::InstrumentScope instruments_;
  trading::MockTradingAdapter mock_;
  trading::SimulatedFuturesExchange futures_;
  trading::CtpTradingAdapter ctp_;
  trading::TradingAdapter* selected_adapter_ = &mock_;
  // One client publishes simulator input for the gateway generation.
  uint64_t simulation_client_id_ = 0;
  std::array<model::QuoteTick, md::kMaxMarketDataInstruments>
      simulation_quotes_ = {};
  Adapter adapter_;
  // Adapters and dictionary outlive the engine, including teardown
  // after partial initialization.
  trading::TradingEngine trading_;
  std::array<Client, trading::kMaxClients> clients_;
  std::array<OrderOwner, trading::kMaxTradingOrders> owners_ = {};
  std::array<uint16_t, kOwnerLookupCapacity> owner_lookup_{};
  trading::AccountView account_;
  uint64_t generation_ = 0;
  bool stopping_ = false;
  uint64_t stop_time_ = 0;
  size_t cancel_cursor_ = 0;
  bool account_dirty_ = false;
  bool initialized_ = false;
  char error_detail_[768] = {};
};

}  // namespace yquant::tradingd
#endif  // YQUANT_TRADINGD_SERVER_H_
