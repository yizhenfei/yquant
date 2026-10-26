#ifndef YQUANT_TRADINGD_SERVER_H_
#define YQUANT_TRADINGD_SERVER_H_

// Assembles the trading gateway. Included only by its entry point,
// implementation and tests; reusable libraries do not depend on this header.

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <span>

#include "tradingd/config.h"
#include "tradingd/journal.h"
#include "tradingd/mysql_archive.h"
#include "yquant/base/monotonic_clock.h"
#include "yquant/md/instrument_scope.h"
#include "yquant/trading/connection.h"
#include "yquant/trading/ctp_trading_adapter.h"
#include "yquant/trading/mock_trading_adapter.h"
#include "yquant/trading/simulated_futures_exchange.h"
#include "yquant/trading/trading_engine.h"
#include "yquant/trading/trading_persistence.h"

namespace yquant::tradingd {

// Single-thread shared order gateway. The clock outlives this object. Construct
// on the heap because the client and order arrays are large. Init prepares
// account storage; accepting a connection allocates its bounded buffers.
// Established order processing does not allocate or wait for WAL sync; mapping
// stores can still incur kernel page/dirty-pressure delays. Cold client
// registration persists identity before Welcome. Persistent identities remain
// retained across restarts, including after EOF.
class Server final : private trading::TradingMutationSink {
 public:
  // Borrows a non-null clock that must outlive this server.
  explicit Server(const base::MonotonicClock* clock) noexcept;
  ~Server() noexcept override;
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  // Initializes a new account or recovers configured persistent state before
  // opening the listener. Failure releases partial resources.
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
  // Cold orderly shutdown: requires drained orders, waits for a final checked
  // checkpoint and background sync. Failure retains diagnostics and state
  // files.
  [[nodiscard]] base::Status Finish() noexcept;

 private:
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
  // timestamps/flags are runtime-only; offline identities still occupy
  // capacity.
  struct Client {
    trading::Connection connection;
    uint64_t id = 0;
    uint64_t session_id = 0;
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
    // Closes the transport and clears the retained identity and order
    // namespace.
    void Reset() noexcept;
  };

  // Dense ledger slot to stable core ID and retained client/local ownership.
  struct OrderOwner {
    model::ClientOrderId core_id;
    size_t client = 0;
    size_t local = 0;
  };

  // Forwards requests to the selected adapter and attaches persistent identity.
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
  // Authenticates and persists a new client before sending its account view.
  base::Status HandleHello(size_t index,
                           const trading::Message& message) noexcept;
  // Validates the simulation publisher and updates quote-driven valuations.
  base::Status HandleSimulationQuote(size_t index,
                                     const trading::Message& message,
                                     uint64_t now) noexcept;
  // Submits a client-local order or persists and sends its rejection.
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
  // Restores runtime values after transports, workers and account state close.
  void ResetRuntimeState() noexcept;
  // Clears persistence bookkeeping after workers have joined.
  void ResetPersistenceState() noexcept;
  // All business methods below run on the server thread. The checkpoint worker
  // owns only an immutable encoded buffer between task publication/completion.
  // Missing IDs return owners_.size(); lookup is bounded and allocation-free.
  size_t OwnerIndex(model::ClientOrderId id) const noexcept;
  // Publishes one previously unused dense slot in the fixed identity lookup.
  void BindOwner(size_t slot, const OrderOwner& owner) noexcept;
  // Records a fact before ledger mutation; new-risk failure preserves state.
  base::Status BeforeCommit(
      const trading::TradingMutation& mutation) noexcept override;
  // Same copy boundary for mock sends/reports; context is this server.
  static base::Status ObserveMock(void*,
                                  const trading::MockTradingMutation&) noexcept;
  // Rebuilds one checked fact without invoking historical external actions.
  static base::Status ReplayRecord(void*, uint16_t, uint64_t,
                                   std::span<const std::byte>) noexcept;
  // Failure marks a permanent log gap. New risk fails; existing facts may
  // still commit in memory for controlled shutdown and subsequent
  // reconciliation.
  base::Status Append(uint16_t type, std::span<const std::byte> payload,
                      bool new_risk) noexcept;
  // Cold atomic control-file operations, serialized with checkpoint writes.
  // The profile freezes every active InstrumentDefinition field before use.
  base::Status SaveIdentity() noexcept;
  // Cold load validates the complete account/catalog profile and retained
  // identities. Legacy ID/precision-only profiles are rejected.
  base::Status LoadIdentity() noexcept;
  // Persists namespace and ownership before Welcome; failure closes new risk.
  base::Status RegisterClient(size_t index) noexcept;
  // Holds LOCK throughout load/replay/reconcile; starts workers only after
  // durable identity allocation. Failure is cleaned up by Close().
  base::Status StartPersistence() noexcept;
  // Saves immutable input and links the previous archive run before listening.
  // Startup-only; failure leaves durable diagnostic files for operator review.
  base::Status PublishArchiveBaseline(bool initialized) noexcept;
  // Cold final boundary; called only after the prefix has been synchronized.
  base::Status SaveArchiveEnd(uint64_t run, uint64_t lsn) noexcept;
  // Resolves the complete recovered simulation prefix and drains old orders.
  // No independent counter evidence exists for a lost simulation WAL tail.
  // Applies the retained mock report prefix without sending requests.
  base::Status ReplayMockHistory() noexcept;
  // Rejects send intents absent from the mock's recovered history.
  base::Status RejectAbsentMockOrders(uint64_t now) noexcept;
  // Resolves cancel intents and queues cancels for recovered working orders.
  base::Status ScheduleRecoveredCancels(uint64_t now) noexcept;
  // Drains recovered orders by advancing only the mock schedule.
  base::Status DrainRecoveredMock(uint64_t now) noexcept;
  base::Status RecoverMock() noexcept;
  // Owner-only coherent encoding; buffer must not be owned by the worker.
  base::Status CaptureCheckpoint(uint64_t now) noexcept;
  // Loads into an initializing server; Close() releases state on failure.
  base::Status LoadCheckpoint(uint64_t& stream, uint64_t& covered) noexcept;
  // Cold/worker: durable snapshot then atomic CURRENT publication; the owner
  // keeps buffer, cursor and identity immutable until completion.
  base::Status PublishCheckpoint() noexcept;
  // Worker reads only the published immutable buffer and atomic controls.
  static void* CheckpointEntry(void*) noexcept;
  // Cold wait; owner must not overwrite or free an in-flight image.
  void WaitCheckpoint() noexcept;
  // Nonwaiting owner poll; captures at most one image when its interval is due.
  void PollCheckpoint(uint64_t now) noexcept;
  // Preserves local rejection/sequence facts outside the ledger mutation path.
  base::Status PersistRejection(size_t client, size_t local,
                                const trading::Message& message) noexcept;
  // Completes/join before Journal close or payload destruction. Idempotent.
  void StopCheckpoint() noexcept;

  const base::MonotonicClock& clock_;
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
  Journal journal_;
  MysqlArchive mysql_archive_;
  Adapter adapter_;
  // Adapters, dictionary and journal outlive the engine, including teardown
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
  uint64_t run_id_ = 0;
  std::array<uint64_t, 2> ledger_id_{};
  uint64_t last_namespace_ = 0;
  // Cold-leased bounds are persisted before any ID in the block is used.
  uint64_t order_id_highwater_ = 0;
  uint64_t cancel_id_highwater_ = 0;
  // A request callback borrows this ownership context; trading::kMaxClients
  // means the mutation identifies an existing order whose owner is looked up
  // instead.
  uint64_t context_client_ = trading::kMaxClients;
  uint64_t context_local_ = 0;
  model::CancelRequestId context_cancel_;
  uint64_t last_record_time_ = 0;
  bool degraded_ = false;
  bool archive_disk_pressure_ = false;
  bool replaying_ = false;
  base::Status persistence_failure_;
  char persistence_detail_[256] = {};
  std::unique_ptr<std::byte[]> checkpoint_;
  size_t checkpoint_size_ = 0;
  uint64_t checkpoint_lsn_ = 0;
  JournalCursor checkpoint_cursor_;
  JournalCursor replay_cursor_;
  uint64_t next_snapshot_at_ = 0;
  pthread_t checkpoint_thread_{};
  bool checkpoint_thread_started_ = false;
  std::atomic<bool> checkpoint_pending_{false};
  std::atomic<bool> checkpoint_stop_{false};
  std::atomic<bool> checkpoint_failed_{false};

  bool initialized_ = false;
  char error_detail_[768] = {};
};

}  // namespace yquant::tradingd
#endif  // YQUANT_TRADINGD_SERVER_H_
