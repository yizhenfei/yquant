#ifndef YQUANT_TRADINGD_SERVER_STORAGE_H_
#define YQUANT_TRADINGD_SERVER_STORAGE_H_

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <span>

#include "tradingd/journal.h"
#include "tradingd/mysql_archive.h"
#include "tradingd/server.h"
#include "yquant/market/instrument_scope.h"
#include "yquant/trading/connection.h"
#include "yquant/trading/ctp_trading_adapter.h"
#include "yquant/trading/mock_trading_adapter.h"
#include "yquant/trading/simulated_futures_exchange.h"
#include "yquant/trading/trading_engine.h"
#include "yquant/trading/trading_persistence.h"

namespace yquant::tradingd {

inline constexpr size_t kOwnerLookupCapacity = 2 * trading::kMaxTradingOrders;
static_assert((kOwnerLookupCapacity & (kOwnerLookupCapacity - 1)) == 0);

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
};

// Dense ledger slot to stable core ID and retained client/local ownership.
struct OrderOwner {
  model::ClientOrderId core_id;
  size_t client = 0;
  size_t local = 0;
};

// Main-thread gateway state. Only explicitly atomic checkpoint controls and
// Journal watermarks are shared; the checkpoint payload is immutable in flight.
struct Server::Storage final : trading::TradingMutationSink {
  explicit Storage(const base::MonotonicClock& clock) noexcept
      : clock_(clock),
        ctp(&instruments),
        adapter(*this),
        trading(&clock, &instruments, &adapter) {}
  const base::MonotonicClock& clock_;
  ServerConfig config;
  trading::Listener listener;
  market::InstrumentScope instruments;
  trading::MockTradingAdapter mock;
  trading::SimulatedFuturesExchange futures;
  trading::CtpTradingAdapter ctp;
  trading::TradingAdapter* selected_adapter = &mock;
  // One client publishes simulator input for the gateway generation.
  uint64_t simulation_client_id = 0;
  std::array<model::QuoteTick, market::kMaxMarketDataInstruments>
      simulation_quotes = {};
  // Attaches the stable adapter request key without changing the common adapter
  // ABI.
  struct Adapter final : trading::TradingAdapter {
    explicit Adapter(Storage& storage) noexcept : storage(storage) {}
    base::Status Prepare(const trading::TradingConfig& c) noexcept override {
      storage.selected_adapter =
          c.adapter == trading::TradingAdapterKind::kCtp
              ? static_cast<trading::TradingAdapter*>(&storage.ctp)
          : c.adapter == trading::TradingAdapterKind::kSimulatedFutures
              ? static_cast<trading::TradingAdapter*>(&storage.futures)
              : static_cast<trading::TradingAdapter*>(&storage.mock);
      return storage.selected_adapter->Prepare(c);
    }
    base::Status Start(uint64_t now) noexcept override {
      return storage.selected_adapter->Start(now);
    }
    void BeginStopping() noexcept override {
      storage.selected_adapter->BeginStopping();
    }
    base::Status ReadAccountSnapshot(
        trading::TradingAccountSnapshot& out) noexcept override {
      return storage.selected_adapter->ReadAccountSnapshot(out);
    }
    base::Status ReadBrokerAccountSnapshot(
        trading::BrokerAccountSnapshot& out) noexcept override {
      return storage.selected_adapter->ReadBrokerAccountSnapshot(out);
    }
    base::Status SendOrder(model::ClientOrderId id,
                           const model::OrderRequest& request,
                           uint64_t now) noexcept override;
    base::Status SendCancel(model::ClientOrderId id,
                            const model::AdapterOrderId& broker,
                            model::CancelRequestId cancel,
                            uint64_t now) noexcept override {
      return storage.selected_adapter->SendCancel(id, broker, cancel, now);
    }
    trading::TradingPollResult Poll(
        uint64_t now, model::TradingEvent& out) noexcept override {
      return storage.selected_adapter->Poll(now, out);
    }
    std::optional<uint64_t> next_deadline() const noexcept override {
      return storage.selected_adapter->next_deadline();
    }
    bool is_drained() const noexcept override {
      return storage.selected_adapter->is_drained();
    }
    base::Status Stop() noexcept override {
      return storage.selected_adapter->Stop();
    }
    const char* error_detail() const noexcept override {
      return storage.selected_adapter->error_detail();
    }
    Storage& storage;
  } adapter;
  // The engine borrows the wrapper, which borrows the concrete adapters.
  // Declaration order keeps every adapter alive through engine destruction,
  // including teardown after partial initialization.
  trading::TradingEngine trading;
  std::array<Client, trading::kMaxClients> clients;
  std::array<OrderOwner, trading::kMaxTradingOrders> owners = {};
  std::array<uint16_t, kOwnerLookupCapacity> owner_lookup{};
  trading::AccountView account;
  uint64_t generation = 0;
  bool stopping = false;
  uint64_t stop_time = 0;
  size_t cancel_cursor = 0;
  bool account_dirty = false;
  Journal journal;
  MysqlArchive mysql_archive;
  uint64_t run_id = 0;
  std::array<uint64_t, 2> ledger_id{};
  uint64_t last_namespace = 0;
  // Cold-leased bounds are persisted before any ID in the block is used.
  uint64_t order_id_highwater = 0;
  uint64_t cancel_id_highwater = 0;
  // A request callback borrows this ownership context; trading::kMaxClients
  // means the mutation identifies an existing order whose owner is looked up
  // instead.
  uint64_t context_client = trading::kMaxClients;
  uint64_t context_local = 0;
  model::CancelRequestId context_cancel;
  uint64_t last_record_time = 0;
  bool degraded = false;
  bool archive_disk_pressure = false;
  bool replaying = false;
  base::Status persistence_failure;
  char persistence_detail[256] = {};
  std::unique_ptr<std::byte[]> checkpoint;
  size_t checkpoint_size = 0;
  uint64_t checkpoint_lsn = 0;
  JournalCursor checkpoint_cursor;
  JournalCursor replay_cursor;
  uint64_t next_snapshot_at = 0;
  pthread_t checkpoint_thread{};
  bool checkpoint_thread_started = false;
  std::atomic<bool> checkpoint_pending{false};
  std::atomic<bool> checkpoint_stop{false};
  std::atomic<bool> checkpoint_failed{false};

  // All business methods below run on the server thread. The checkpoint worker
  // owns only an immutable encoded buffer between task publication/completion.
  // Missing IDs return owners.size(); lookup is bounded and allocation-free.
  size_t OwnerIndex(model::ClientOrderId id) const noexcept;
  // Publishes one previously unused dense slot in the fixed identity lookup.
  void BindOwner(size_t slot, const OrderOwner& owner) noexcept;
  // Copies an owned fact before ledger mutation; new-risk failure rolls back.
  base::Status BeforeCommit(
      const trading::TradingMutation& mutation) noexcept override;
  // Same copy boundary for mock sends/reports; context is this live Storage.
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
  // durable identity allocation. Caller discards Storage on any failure.
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
  // Cold install into initializing Storage, which is discarded on failure.
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
};

}  // namespace yquant::tradingd
#endif  // YQUANT_TRADINGD_SERVER_STORAGE_H_
