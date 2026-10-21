#include "tradingd/mysql_archive_store.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <string_view>

#include "tradingd/archive_projection.h"
#include "tradingd/state_codec.h"

#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
#include <mysql.h>
#endif

namespace yquant::tradingd {
namespace {

// SQL identifiers and statements below are constants. Only typed parameters
// carry ledger data; opaque IDs are bound as binary, never as SQL text.
#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
pthread_once_t mysql_library_once = PTHREAD_ONCE_INIT;
int mysql_library_status = 0;

void InitializeMysqlLibrary() noexcept {
  mysql_library_status = mysql_library_init(0, nullptr, nullptr);
}

// C API per-thread state survives reconnects and is released when the owning
// archive worker exits. Library initialization is serialized across ledgers.
struct MysqlThreadContext {
  MysqlThreadContext() noexcept : status(mysql_thread_init()) {}
  ~MysqlThreadContext() noexcept {
    if (status == 0) mysql_thread_end();
  }
  int status;
};

constexpr unsigned kCancelled = 65535;
constexpr std::uint64_t kSchemaVersion = 1;
constexpr std::string_view kSchemaChecksum =
    "yquant-trading-archive-v1-20261021";

struct Value {
  enum class Kind { kUnsigned, kSigned, kBytes, kNull };
  Kind kind = Kind::kUnsigned;
  std::uint64_t unsigned_value = 0;
  std::int64_t signed_value = 0;
  std::span<const std::byte> bytes;
  explicit Value(std::uint64_t value) noexcept : unsigned_value(value) {}
  explicit Value(std::int64_t value) noexcept
      : kind(Kind::kSigned), signed_value(value) {}
  explicit Value(std::span<const std::byte> value) noexcept
      : kind(Kind::kBytes), bytes(value) {}
  explicit Value(std::string_view value) noexcept
      : Value(std::as_bytes(std::span(value.data(), value.size()))) {}
  Value() noexcept : kind(Kind::kNull) {}
};

bool IsTransient(unsigned code) noexcept {
  // Lock wait/deadlock, connection failures and server disconnects. Constraint,
  // permission and schema failures require operator correction.
  return code == kCancelled || code == 1205 || code == 1213 || code == 1040 ||
         code == 1053 || code == 2002 || code == 2003 || code == 2006 ||
         code == 2013 || code == 2055;
}

std::array<std::byte, 16> LedgerBytes(
    const std::array<std::uint64_t, 2>& id) noexcept {
  std::array<std::byte, 16> bytes{};
  for (std::size_t word = 0; word < id.size(); ++word)
    for (unsigned i = 0; i < 8; ++i)
      bytes[word * 8 + i] = std::byte(id[word] >> (56 - i * 8));
  return bytes;
}

std::array<std::byte, 8> CoreNamespace(std::uint64_t core) noexcept {
  std::array<std::byte, 8> bytes{};
  for (unsigned i = 0; i < 8; ++i) bytes[i] = std::byte(core >> (56 - i * 8));
  return bytes;
}
#endif

}  // namespace

struct MysqlArchiveStore::Storage {
#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
  MYSQL* connection = nullptr;
  bool (*cancelled)(void*) noexcept = nullptr;
  void* cancellation_context = nullptr;
  unsigned error = 0;
  char detail[256] = {};
  bool transaction = false;
  std::array<std::byte, 16> ledger{};
  MysqlArchiveProgress progress;
  std::uint64_t pending_run = 0;
  std::uint64_t pending_lsn = 0;
  std::uint64_t final_lsn = UINT64_MAX;

  void Error(unsigned code, const char* message) noexcept {
    error = code;
    std::snprintf(detail, sizeof(detail), "MySQL error %u: %.200s", code,
                  message);
  }
  bool Cancelled() noexcept {
    if (cancelled == nullptr || !cancelled(cancellation_context)) return false;
    Error(kCancelled, "archive operation cancelled");
    return true;
  }
  bool Raw(const char* sql) noexcept {
    if (Cancelled()) return false;
    if (mysql_real_query(connection, sql, std::strlen(sql)) == 0) return true;
    Error(mysql_errno(connection), mysql_error(connection));
    return false;
  }

  // Binds one bounded parameter array; long BLOBs are transmitted in chunks.
  // Results contain only integral validation flags or progress coordinates.
  // SELECT results are fully buffered before statement closure.
  bool Query(const char* sql, std::initializer_list<Value> params,
             std::span<std::uint64_t> output = {},
             bool* found = nullptr) noexcept {
    error = 0;
    if (Cancelled()) return false;
    MYSQL_STMT* statement = mysql_stmt_init(connection);
    if (statement == nullptr) {
      Error(mysql_errno(connection), mysql_error(connection));
      return false;
    }
    bool ok = mysql_stmt_prepare(statement, sql, std::strlen(sql)) == 0;
    if (!ok) {
      Error(mysql_stmt_errno(statement), mysql_stmt_error(statement));
      mysql_stmt_close(statement);
      return false;
    }
    constexpr std::size_t kMaxParameters = 32;
    constexpr std::size_t kLongChunkBytes = 64 * 1024;
    std::array<MYSQL_BIND, kMaxParameters> bindings{};
    std::array<unsigned long, kMaxParameters> lengths{};
    if (params.size() > bindings.size() ||
        mysql_stmt_param_count(statement) != params.size()) {
      mysql_stmt_close(statement);
      Error(0, "archive statement parameter mismatch");
      return false;
    }
    std::size_t index = 0;
    for (const auto& value : params) {
      auto& bind = bindings[index];
      switch (value.kind) {
        case Value::Kind::kUnsigned:
          bind.buffer_type = MYSQL_TYPE_LONGLONG;
          bind.buffer = const_cast<std::uint64_t*>(&value.unsigned_value);
          bind.is_unsigned = true;
          break;
        case Value::Kind::kSigned:
          bind.buffer_type = MYSQL_TYPE_LONGLONG;
          bind.buffer = const_cast<std::int64_t*>(&value.signed_value);
          break;
        case Value::Kind::kBytes:
          bind.buffer_type = MYSQL_TYPE_LONG_BLOB;
          bind.buffer = const_cast<std::byte*>(value.bytes.data());
          bind.buffer_length = value.bytes.size();
          lengths[index] = value.bytes.size();
          bind.length = &lengths[index];
          break;
        case Value::Kind::kNull:
          bind.buffer_type = MYSQL_TYPE_NULL;
          break;
      }
      ++index;
    }
    if (ok && !params.size()) {
      // No input bindings are required for version/session validation.
    } else if (ok) {
      ok = mysql_stmt_bind_param(statement, bindings.data()) == 0;
    }
    index = 0;
    for (const auto& value : params) {
      if (value.kind == Value::Kind::kBytes &&
          value.bytes.size() > kLongChunkBytes) {
        for (std::size_t offset = 0; ok && offset < value.bytes.size();) {
          const auto length =
              std::min(kLongChunkBytes, value.bytes.size() - offset);
          ok = mysql_stmt_send_long_data(
                   statement, static_cast<unsigned>(index),
                   reinterpret_cast<const char*>(value.bytes.data() + offset),
                   length) == 0;
          offset += length;
        }
      }
      ++index;
    }
    if (ok) ok = mysql_stmt_execute(statement) == 0;
    if (ok && !output.empty()) {
      std::array<MYSQL_BIND, kMaxParameters> results{};
      if (output.size() > results.size() ||
          mysql_stmt_field_count(statement) != output.size()) {
        Error(0, "archive result shape mismatch");
        mysql_stmt_close(statement);
        return false;
      }
      for (std::size_t i = 0; i < output.size(); ++i) {
        results[i].buffer_type = MYSQL_TYPE_LONGLONG;
        results[i].buffer = &output[i];
        results[i].buffer_length = sizeof(output[i]);
        results[i].is_unsigned = true;
      }
      ok = mysql_stmt_bind_result(statement, results.data()) == 0 &&
           mysql_stmt_store_result(statement) == 0;
      if (ok) {
        const auto rows = mysql_stmt_num_rows(statement);
        if (rows > 1) {
          Error(0, "archive query returned multiple identities");
          mysql_stmt_close(statement);
          return false;
        }
        const auto result = mysql_stmt_fetch(statement);
        if (found != nullptr) *found = result == 0;
        ok = result == 0 || result == MYSQL_NO_DATA;
      }
    }
    if (!ok) Error(mysql_stmt_errno(statement), mysql_stmt_error(statement));
    // Strict mode rejects conversion errors. Also reject warnings so schema
    // drift cannot silently truncate data even with a changed session mode.
    if (ok && mysql_warning_count(connection) != 0) {
      Error(0, "archive statement produced a warning");
      ok = false;
    }
    mysql_stmt_close(statement);
    return ok;
  }

  bool Exact(const char* sql, std::initializer_list<Value> values) noexcept {
    std::uint64_t match = 0;
    bool found = false;
    if (!Query(sql, values, {&match, 1}, &found)) return false;
    if (found && match == 1) return true;
    Error(0, "immutable archive identity/content conflict");
    return false;
  }

  bool Identity(const ArchiveProjection& projection, bool create) noexcept {
    ledger = LedgerBytes(projection.ledger_id());
    if (create &&
        !Query("INSERT INTO ledgers (ledger_id,account_key,accounting_version) "
               "VALUES (?,?,1)",
               {Value(ledger), Value(projection.account_key())}) &&
        error != 1062)
      return false;
    // Acquire the ledger's exclusive lock with a current read before any
    // consistent read establishes a REPEATABLE READ snapshot. After waiting for
    // another writer, both progress and its newly installed run/facts must be
    // visible to this transaction. Duplicate INSERT alone takes a shared lock.
    return Exact(create ? "SELECT account_key=? AND accounting_version=1 "
                          "FROM ledgers WHERE ledger_id=? FOR UPDATE"
                        : "SELECT account_key=? AND accounting_version=1 "
                          "FROM ledgers WHERE ledger_id=?",
                 {Value(projection.account_key()), Value(ledger)});
  }

  bool ReadProgress(MysqlArchiveProgress& out, bool lock) noexcept {
    std::array<std::uint64_t, 7> fields{};
    bool found = false;
    if (!Query(
            lock ? "SELECT run_id,baseline_installed,lsn,segment_no,"
                   "segment_offset,projection_version,1 FROM archive_progress "
                   "WHERE ledger_id=? FOR UPDATE"
                 : "SELECT run_id,baseline_installed,lsn,segment_no,"
                   "segment_offset,projection_version,1 FROM archive_progress "
                   "WHERE ledger_id=?",
            {Value(ledger)}, fields, &found))
      return false;
    if (found && fields[5] != kSchemaVersion) {
      Error(0, "unsupported archive projection version");
      return false;
    }
    MysqlArchiveProgress result;
    result.exists = found && fields[1] != 0;
    result.baseline_installed = result.exists;
    result.cursor = {fields[0], fields[3], fields[4], fields[2]};
    out = result;
    return true;
  }
#endif
};

MysqlArchiveStore::~MysqlArchiveStore() noexcept { Close(); }

base::Status MysqlArchiveStore::Fail(base::ErrorCode code, const char* detail,
                                     bool retry) noexcept {
  retriable_ = retry;
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
  return {code, {}};
}

base::Status MysqlArchiveStore::DatabaseFailure() noexcept {
#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
  return Fail(storage_->error == 0 ? base::ErrorCode::kCorrupt
                                   : base::ErrorCode::kIoError,
              storage_->detail, IsTransient(storage_->error));
#else
  return Fail(base::ErrorCode::kUnsupported,
              "MySQL support is not enabled in this build");
#endif
}

base::Status MysqlArchiveStore::Connect(
    const MysqlArchiveOptions& options) noexcept {
  Close();
  retriable_ = false;
  error_detail_[0] = '\0';
#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
  pthread_once(&mysql_library_once, InitializeMysqlLibrary);
  if (mysql_library_status != 0)
    return Fail(base::ErrorCode::kResourceExhausted,
                "cannot initialize MySQL client library");
  thread_local MysqlThreadContext thread_context;
  if (thread_context.status != 0)
    return Fail(base::ErrorCode::kResourceExhausted,
                "cannot initialize MySQL client thread");
  storage_ = new (std::nothrow) Storage;
  if (storage_ == nullptr)
    return Fail(base::ErrorCode::kResourceExhausted,
                "cannot allocate MySQL archive connection");
  storage_->cancelled = cancelled_;
  storage_->cancellation_context = cancellation_context_;
  storage_->connection = mysql_init(nullptr);
  if (storage_->connection == nullptr) {
    Close();
    return Fail(base::ErrorCode::kResourceExhausted,
                "cannot initialize MySQL client");
  }
  MYSQL* connection = storage_->connection;
  unsigned timeout = options.timeout_seconds;
  unsigned protocol = MYSQL_PROTOCOL_TCP;
  // Local TCP permits deployments without certificates. Every remote host
  // requires VERIFY_IDENTITY and an explicit trust anchor.
  const bool local = std::strcmp(options.host, "127.0.0.1") == 0 ||
                     std::strcmp(options.host, "::1") == 0 ||
                     std::strcmp(options.host, "localhost") == 0;
  if (!local && options.tls_ca[0] == '\0') {
    Close();
    return Fail(base::ErrorCode::kInvalidArgument,
                "remote MySQL archive requires tls_ca");
  }
  enum mysql_ssl_mode ssl =
      options.tls_ca[0] != '\0' ? SSL_MODE_VERIFY_IDENTITY : SSL_MODE_PREFERRED;
  bool report_truncation = true;
  if (mysql_options(connection, MYSQL_OPT_CONNECT_TIMEOUT, &timeout) != 0 ||
      mysql_options(connection, MYSQL_OPT_READ_TIMEOUT, &timeout) != 0 ||
      mysql_options(connection, MYSQL_OPT_WRITE_TIMEOUT, &timeout) != 0 ||
      mysql_options(connection, MYSQL_OPT_PROTOCOL, &protocol) != 0 ||
      mysql_options(connection, MYSQL_OPT_SSL_MODE, &ssl) != 0 ||
      mysql_options(connection, MYSQL_REPORT_DATA_TRUNCATION,
                    &report_truncation) != 0 ||
      (options.tls_ca[0] != '\0' &&
       mysql_options(connection, MYSQL_OPT_SSL_CA, options.tls_ca) != 0)) {
    storage_->Error(mysql_errno(connection), mysql_error(connection));
    const auto status = DatabaseFailure();
    Close();
    return status;
  }
  const char* password =
      options.password_env[0] == '\0' ? "" : std::getenv(options.password_env);
  if (password == nullptr) {
    Close();
    return Fail(base::ErrorCode::kInvalidArgument,
                "MySQL archive password environment variable is unset");
  }
  if (mysql_real_connect(connection, options.host, options.user, password,
                         options.database, options.port, nullptr,
                         0) == nullptr) {
    storage_->Error(mysql_errno(connection), mysql_error(connection));
    const auto status = DatabaseFailure();
    Close();
    return status;
  }
  if (mysql_get_server_version(connection) < 80400) {
    Close();
    return Fail(base::ErrorCode::kUnsupported,
                "MySQL archive requires server version 8.4 or later");
  }
  if (!storage_->Raw("SET SESSION sql_mode='STRICT_ALL_TABLES,NO_ZERO_DATE,"
                     "NO_ZERO_IN_DATE,ERROR_FOR_DIVISION_BY_ZERO,"
                     "NO_ENGINE_SUBSTITUTION'") ||
      !storage_->Raw("SET SESSION time_zone='+00:00'") ||
      !storage_->Raw(
          "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ") ||
      !storage_->Exact("SELECT checksum=? FROM archive_schema WHERE version=1",
                       {Value(kSchemaChecksum)}) ||
      !storage_->Exact(
          "SELECT COUNT(*)=12 AND SUM(ENGINE='InnoDB')=12 FROM "
          "information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND "
          "TABLE_NAME IN ('archive_schema','ledgers','archive_runs',"
          "'strategy_sessions','trade_events','archive_gaps',"
          "'archive_progress','orders','fills','order_updates','account_cash',"
          "'account_positions')",
          {}) ||
      !storage_->Exact("SELECT @@GLOBAL.innodb_flush_log_at_trx_commit=1 AND "
                       "(NOT @@GLOBAL.log_bin OR @@GLOBAL.sync_binlog=1)",
                       {})) {
    const auto status = DatabaseFailure();
    Close();
    return status;
  }
  return {};
#else
  (void)options;
  return DatabaseFailure();
#endif
}

void MysqlArchiveStore::SetCancellation(bool (*cancelled)(void*) noexcept,
                                        void* context) noexcept {
  cancelled_ = cancelled;
  cancellation_context_ = context;
#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
  if (storage_ != nullptr) {
    storage_->cancelled = cancelled;
    storage_->cancellation_context = context;
  }
#endif
}

void MysqlArchiveStore::Close() noexcept {
  if (storage_ == nullptr) return;
#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
  if (storage_->connection != nullptr) mysql_close(storage_->connection);
#endif
  delete storage_;
  storage_ = nullptr;
}

base::Status MysqlArchiveStore::GetProgress(
    const ArchiveProjection& projection, MysqlArchiveProgress& out) noexcept {
#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
  if (storage_ == nullptr || storage_->transaction)
    return Fail(base::ErrorCode::kInvalidState, "MySQL connection is not idle");
  storage_->ledger = LedgerBytes(projection.ledger_id());
  // An absent ledger has no committed progress. Avoid creating it outside the
  // first baseline transaction, preserving all-or-nothing publication.
  std::uint64_t match = 0;
  bool found = false;
  if (!storage_->Query(
          "SELECT account_key=? AND accounting_version=1 "
          "FROM ledgers WHERE ledger_id=?",
          {Value(projection.account_key()), Value(storage_->ledger)},
          {&match, 1}, &found))
    return DatabaseFailure();
  if (found && match != 1)
    return Fail(base::ErrorCode::kCorrupt, "archive ledger identity conflict");
  if (!storage_->ReadProgress(out, false)) return DatabaseFailure();
  return {};
#else
  (void)projection;
  (void)out;
  return DatabaseFailure();
#endif
}

base::Status MysqlArchiveStore::Begin(const ArchiveProjection& projection,
                                      MysqlArchiveProgress& out) noexcept {
#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
  if (storage_ == nullptr || storage_->transaction)
    return Fail(base::ErrorCode::kInvalidState, "MySQL connection is not idle");
  if (!storage_->Raw("START TRANSACTION")) return DatabaseFailure();
  storage_->transaction = true;
  if (!storage_->Identity(projection, true)) return DatabaseFailure();
  if (!storage_->Query("INSERT INTO archive_progress (ledger_id,run_id,"
                       "baseline_installed,lsn,segment_no,segment_offset,"
                       "projection_version) VALUES (?,0,0,0,0,0,1)",
                       {Value(storage_->ledger)}) &&
      storage_->error != 1062)
    return DatabaseFailure();
  MysqlArchiveProgress progress;
  if (!storage_->ReadProgress(progress, true)) return DatabaseFailure();
  storage_->progress = progress;
  storage_->pending_run = progress.cursor.stream_id;
  storage_->pending_lsn = progress.cursor.lsn;
  storage_->final_lsn = UINT64_MAX;
  if (progress.exists) {
    bool found = false;
    if (!storage_->Query(
            "SELECT COALESCE(final_lsn,18446744073709551615) FROM archive_runs "
            "WHERE ledger_id=? AND run_id=?",
            {Value(storage_->ledger), Value(progress.cursor.stream_id)},
            {&storage_->final_lsn, 1}, &found))
      return DatabaseFailure();
    if (!found || (storage_->final_lsn != UINT64_MAX &&
                   storage_->final_lsn != progress.cursor.lsn))
      return Fail(
          base::ErrorCode::kCorrupt,
          "archive run final boundary conflicts with committed progress");
  }
  out = progress;
  return {};
#else
  (void)projection;
  (void)out;
  return DatabaseFailure();
#endif
}

base::Status MysqlArchiveStore::InstallRun(std::span<const std::byte> baseline,
                                           const ArchiveProjection& projection,
                                           std::uint64_t previous_run,
                                           std::uint64_t previous_end,
                                           bool history_gap) noexcept {
#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
  if (storage_ == nullptr || !storage_->transaction ||
      projection.last_lsn() != 0 || projection.run_id() == 0 ||
      baseline.empty() || baseline.size() > kJournalMaxSnapshotBytes ||
      previous_run >= projection.run_id() ||
      (previous_run == 0 && previous_end != 0))
    return Fail(base::ErrorCode::kInvalidState,
                "invalid archive baseline install");
  const auto& progress = storage_->progress;
  if (progress.exists && (progress.cursor.stream_id != previous_run ||
                          progress.cursor.lsn != previous_end))
    return Fail(
        base::ErrorCode::kIoError,
        "archive predecessor progress changed; reload database progress", true);
  const auto ledger = Value(storage_->ledger);
  const auto run = Value(projection.run_id());
  if (progress.exists) {
    if (!storage_->Exact(
            "SELECT final_lsn IS NULL OR final_lsn=? FROM archive_runs "
            "WHERE ledger_id=? AND run_id=?",
            {Value(previous_end), ledger, Value(previous_run)}) ||
        !storage_->Query("UPDATE archive_runs SET final_lsn=? "
                         "WHERE ledger_id=? AND run_id=? AND final_lsn IS NULL",
                         {Value(previous_end), ledger, Value(previous_run)}))
      return DatabaseFailure();
  }
  if (!storage_->Exact("SELECT @@max_allowed_packet>=?",
                       {Value(std::uint64_t(baseline.size() + 4096))}))
    return DatabaseFailure();
  if (!storage_->Query("INSERT INTO archive_runs (ledger_id,run_id,"
                       "previous_run_id,previous_end_lsn,baseline_version,"
                       "baseline,history_gap) VALUES (?,?,?,?,1,?,?)",
                       {ledger, run, Value(previous_run), Value(previous_end),
                        Value(baseline), Value(std::uint64_t(history_gap))})) {
    if (storage_->error != 1062 ||
        !storage_->Exact(
            "SELECT previous_run_id=? AND previous_end_lsn=? "
            "AND baseline_version=1 AND baseline=? AND "
            "history_gap=? FROM archive_runs "
            "WHERE ledger_id=? AND run_id=?",
            {Value(previous_run), Value(previous_end), Value(baseline),
             Value(std::uint64_t(history_gap)), ledger, run}))
      return DatabaseFailure();
  }
  if (history_gap) {
    const std::uint64_t reason = progress.exists ? 2 : 1;
    const auto gap_run = previous_run == 0 ? Value() : Value(previous_run);
    const auto gap_end = previous_run == 0 ? Value() : Value(previous_end);
    if (!storage_->Query(
            "INSERT INTO archive_gaps (ledger_id,run_id,gap_no,"
            "reason,previous_run_id,previous_end_lsn,has_baseline) "
            "VALUES (?,?,1,?,?,?,1)",
            {ledger, run, Value(reason), gap_run, gap_end})) {
      if (storage_->error != 1062 ||
          !storage_->Exact("SELECT reason=? AND (previous_run_id <=> ?) AND "
                           "(previous_end_lsn <=> ?) AND has_baseline=1 "
                           "FROM archive_gaps WHERE ledger_id=? AND run_id=? "
                           "AND gap_no=1",
                           {Value(reason), gap_run, gap_end, ledger, run}))
        return DatabaseFailure();
    }
  }
  storage_->pending_run = projection.run_id();
  storage_->pending_lsn = 0;
  storage_->final_lsn = UINT64_MAX;
  return {};
#else
  (void)baseline;
  (void)projection;
  (void)previous_run;
  (void)previous_end;
  (void)history_gap;
  return DatabaseFailure();
#endif
}

base::Status MysqlArchiveStore::AppendEvent(
    std::uint16_t type, std::uint64_t lsn, std::span<const std::byte> payload,
    const ArchiveProjection& projection) noexcept {
#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
  if (storage_ == nullptr || !storage_->transaction ||
      projection.run_id() != storage_->pending_run ||
      projection.last_lsn() != lsn || lsn == 0 ||
      storage_->pending_lsn == UINT64_MAX || lsn != storage_->pending_lsn + 1 ||
      lsn > storage_->final_lsn || type < 1 || type > 3 || payload.empty() ||
      payload.size() > kJournalMaxPayloadBytes)
    return Fail(base::ErrorCode::kCorrupt,
                "archive event does not follow locked continuous progress");
  const auto ledger = Value(storage_->ledger);
  const auto run = Value(projection.run_id());
  const auto sequence = Value(lsn);
  if (!storage_->Query(
          "INSERT INTO trade_events (ledger_id,run_id,lsn,"
          "record_type,encoding_version,payload,monotonic_time_ns) "
          "VALUES (?,?,?,?,1,?,?)",
          {ledger, run, sequence, Value(std::uint64_t(type)), Value(payload),
           Value(projection.last_record_time())})) {
    if (storage_->error != 1062 ||
        !storage_->Exact(
            "SELECT record_type=? AND encoding_version=1 AND "
            "payload=? AND monotonic_time_ns=? FROM trade_events "
            "WHERE ledger_id=? AND run_id=? AND lsn=?",
            {Value(std::uint64_t(type)), Value(payload),
             Value(projection.last_record_time()), ledger, run, sequence}))
      return DatabaseFailure();
  }
  if (projection.has_update()) {
    const auto& change = projection.last_update();
    const auto& update = change.update;
    std::array<std::byte, 144> reason;
    StateWriter writer(reason);
    writer.U64(static_cast<std::uint64_t>(update.reason.code));
    writer.U64(update.reason.truncated ? 1 : 0);
    writer.Bytes(std::as_bytes(
        std::span(update.reason.view().data(), update.reason.view().size())));
    const auto reason_bytes = std::span(reason).first(writer.size());
    if (!storage_->Query(
            "INSERT INTO order_updates (ledger_id,run_id,lsn,"
            "strategy_session_id,local_order_id,update_kind,status,"
            "reason,cumulative_filled_qty,report_sequence,"
            "local_cancel_id,notify_order) VALUES "
            "(?,?,?,?,?,?,?,?,?,?,?,?)",
            {ledger, run, sequence, Value(change.session_id),
             Value(change.local_order_id), Value(std::uint64_t(update.kind)),
             Value(std::uint64_t(update.order.state)), Value(reason_bytes),
             Value(update.order.cumulative_filled_qty),
             Value(update.order.report_sequence),
             Value(update.cancel_request_id.value),
             Value(std::uint64_t(change.notify_order))})) {
      if (storage_->error != 1062 ||
          !storage_->Exact(
              "SELECT strategy_session_id=? AND local_order_id=? AND "
              "update_kind=? AND status=? AND reason=? AND "
              "cumulative_filled_qty=? AND report_sequence=? AND "
              "local_cancel_id=? AND notify_order=? FROM order_updates "
              "WHERE ledger_id=? AND run_id=? AND lsn=?",
              {Value(change.session_id), Value(change.local_order_id),
               Value(std::uint64_t(update.kind)),
               Value(std::uint64_t(update.order.state)), Value(reason_bytes),
               Value(update.order.cumulative_filled_qty),
               Value(update.order.report_sequence),
               Value(update.cancel_request_id.value),
               Value(std::uint64_t(change.notify_order)), ledger, run,
               sequence}))
        return DatabaseFailure();
    }
  }
  storage_->pending_lsn = lsn;
  return {};
#else
  (void)type;
  (void)lsn;
  (void)payload;
  (void)projection;
  return DatabaseFailure();
#endif
}

base::Status MysqlArchiveStore::WriteProjection(
    const ArchiveProjection& projection) noexcept {
#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
  const auto ledger = Value(storage_->ledger);
  const auto run = Value(projection.run_id());
  const auto sequence = Value(projection.last_lsn());
  const bool baseline =
      storage_->pending_run != storage_->progress.cursor.stream_id;
  for (const auto& session : projection.sessions()) {
    if (session.session_id == 0) continue;
    std::array<std::byte, 2048> metadata{};
    StateWriter writer(metadata);
    writer.U64(1);
    writer.U64(session.client_id);
    writer.U64(session.session_id);
    writer.U64(session.instrument_count);
    for (std::size_t i = 0; i < session.instrument_count; ++i) {
      writer.U64(session.instruments[i].id.value);
      writer.U64(session.instruments[i].price_decimals);
    }
    if (!writer.ok())
      return Fail(base::ErrorCode::kCorrupt,
                  "archive session metadata overflow");
    const auto bytes = Value(std::span(metadata).first(writer.size()));
    if (!storage_->Query("INSERT INTO strategy_sessions (ledger_id,"
                         "strategy_session_id,client_id,metadata) VALUES "
                         "(?,?,?,?)",
                         {ledger, Value(session.session_id),
                          Value(session.client_id), bytes})) {
      if (storage_->error != 1062 ||
          !storage_->Exact("SELECT client_id=? AND metadata=? FROM "
                           "strategy_sessions WHERE ledger_id=? "
                           "AND strategy_session_id=?",
                           {Value(session.client_id), bytes, ledger,
                            Value(session.session_id)}))
        return DatabaseFailure();
    }
  }
  for (const auto& order : projection.orders()) {
    if (!baseline && order.source_lsn <= storage_->progress.cursor.lsn)
      continue;
    const auto& value = order.snapshot;
    const auto core_namespace = CoreNamespace(order.core_id.value);
    const auto core =
        order.core_id.value == 0 ? Value() : Value(order.core_id.value);
    // Only mutable query state is upserted. Its immutable ownership/core key
    // must match first, including when the core unique index caused a conflict.
    const auto session = Value(order.session_id);
    const auto local = Value(order.local_order_id);
    std::uint64_t exists = 0;
    bool found = false;
    if (!storage_->Query("SELECT (core_order_id <=> ?) AND instrument_id=? AND "
                         "side=? AND position_effect=? AND limit_price=? AND "
                         "original_qty=? FROM orders "
                         "WHERE ledger_id=? AND strategy_session_id=? "
                         "AND local_order_id=?",
                         {core, Value(value.instrument_id.value),
                          Value(std::uint64_t(value.side)),
                          Value(std::uint64_t(value.position_effect)),
                          Value(value.limit_price), Value(value.original_qty),
                          ledger, session, local},
                         {&exists, 1}, &found))
      return DatabaseFailure();
    if (found && exists != 1)
      return Fail(base::ErrorCode::kCorrupt,
                  "archive order ownership conflict");
    if (!found && !storage_->Query(
                      "INSERT INTO orders (ledger_id,strategy_session_id,"
                      "local_order_id,core_order_id,instrument_id,side,"
                      "position_effect,limit_price,original_qty,"
                      "cumulative_filled_qty,leaves_qty,status,cancel_pending,"
                      "local_cancel_id,report_sequence,broker_namespace,"
                      "broker_order_id,receive_time_ns,source_run_id,"
                      "source_lsn) VALUES "
                      "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                      {ledger,
                       session,
                       local,
                       core,
                       Value(value.instrument_id.value),
                       Value(std::uint64_t(value.side)),
                       Value(std::uint64_t(value.position_effect)),
                       Value(value.limit_price),
                       Value(value.original_qty),
                       Value(value.cumulative_filled_qty),
                       Value(value.leaves_qty),
                       Value(std::uint64_t(value.state)),
                       Value(std::uint64_t(value.cancel_pending)),
                       Value(order.local_cancel_id.value),
                       Value(order.report_sequence),
                       Value(core_namespace),
                       Value(value.broker_order_id.view()),
                       Value(value.receive_time_ns),
                       run,
                       Value(order.source_lsn)}))
      return DatabaseFailure();
    if (found &&
        !storage_->Query(
            "UPDATE orders SET instrument_id=?,side=?,"
            "position_effect=?,limit_price=?,original_qty=?,"
            "cumulative_filled_qty=?,leaves_qty=?,status=?,"
            "cancel_pending=?,local_cancel_id=?,report_sequence=?,"
            "broker_namespace=?,broker_order_id=?,receive_time_ns=?,"
            "source_run_id=?,source_lsn=? WHERE ledger_id=? "
            "AND strategy_session_id=? AND local_order_id=?",
            {Value(value.instrument_id.value), Value(std::uint64_t(value.side)),
             Value(std::uint64_t(value.position_effect)),
             Value(value.limit_price), Value(value.original_qty),
             Value(value.cumulative_filled_qty), Value(value.leaves_qty),
             Value(std::uint64_t(value.state)),
             Value(std::uint64_t(value.cancel_pending)),
             Value(order.local_cancel_id.value), Value(order.report_sequence),
             Value(core_namespace), Value(value.broker_order_id.view()),
             Value(value.receive_time_ns), run, Value(order.source_lsn), ledger,
             session, local}))
      return DatabaseFailure();
  }
  const auto& trading = projection.trading();
  for (std::size_t i = 0; i < trading.fill_count(); ++i) {
    const auto source = projection.FillSourceLsn(i);
    if (!baseline && source <= storage_->progress.cursor.lsn) continue;
    model::Fill fill;
    if (!trading.TryGetFillAt(i, fill))
      return Fail(base::ErrorCode::kCorrupt, "archive fill projection missing");
    const auto* order = projection.FindOrder(fill.client_order_id);
    if (order == nullptr)
      return Fail(base::ErrorCode::kCorrupt, "archive fill has no order owner");
    // Mock execution IDs are unique within a durable core order. The binary
    // big-endian core identity is the complete namespace, without hashing.
    const auto namespace_bytes = CoreNamespace(fill.client_order_id.value);
    const auto name = Value(namespace_bytes);
    const auto execution = Value(fill.execution_id.view());
    const auto time = fill.has_event_time ? Value(fill.event_time_ns) : Value();
    if (!storage_->Query(
            "INSERT INTO fills (ledger_id,execution_namespace,"
            "execution_id,strategy_session_id,local_order_id,"
            "core_order_id,instrument_id,side,position_effect,price,"
            "quantity,broker_order_id,business_time,receive_time_ns,"
            "source_run_id,source_lsn,baseline_discovered) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {ledger, name, execution, Value(order->session_id),
             Value(order->local_order_id), Value(fill.client_order_id.value),
             Value(fill.instrument_id.value), Value(std::uint64_t(fill.side)),
             Value(std::uint64_t(fill.position_effect)), Value(fill.price),
             Value(fill.quantity), Value(fill.broker_order_id.view()), time,
             Value(fill.receive_time_ns), run, Value(source),
             Value(std::uint64_t(baseline))})) {
      // A baseline may rediscover an already archived fill with a different
      // source. Origin metadata remains immutable; business fields must match.
      if (storage_->error != 1062 ||
          !storage_->Exact(
              "SELECT strategy_session_id=? AND local_order_id=? "
              "AND core_order_id=? AND instrument_id=? AND side=? "
              "AND position_effect=? AND price=? AND quantity=? "
              "AND broker_order_id=? AND (business_time <=> ?) "
              "AND receive_time_ns=? FROM fills WHERE ledger_id=? "
              "AND execution_namespace=? AND execution_id=?",
              {Value(order->session_id), Value(order->local_order_id),
               Value(fill.client_order_id.value),
               Value(fill.instrument_id.value), Value(std::uint64_t(fill.side)),
               Value(std::uint64_t(fill.position_effect)), Value(fill.price),
               Value(fill.quantity), Value(fill.broker_order_id.view()), time,
               Value(fill.receive_time_ns), ledger, name, execution}))
        return DatabaseFailure();
    }
  }
  model::CashSnapshot cash;
  if (!trading.TryGetCash(cash))
    return Fail(base::ErrorCode::kCorrupt, "archive cash projection missing");
  if (!storage_->Query(
          "INSERT INTO account_cash (ledger_id,balance,available,"
          "reserved,source_run_id,source_lsn) VALUES (?,?,?,?,?,?) "
          "ON DUPLICATE KEY UPDATE balance=?,available=?,"
          "reserved=?,source_run_id=?,source_lsn=?",
          {ledger, Value(cash.balance), Value(cash.available),
           Value(cash.reserved), run, sequence, Value(cash.balance),
           Value(cash.available), Value(cash.reserved), run, sequence}))
    return DatabaseFailure();
  // Replace positions atomically at every commit. Query state is bounded by
  // the immutable catalog; zero positions remain visible for known instruments.
  if (!storage_->Query("DELETE FROM account_positions WHERE ledger_id=?",
                       {ledger}))
    return DatabaseFailure();
  for (const auto& instrument : projection.instruments()) {
    model::PositionSnapshot position;
    if (!trading.TryGetPosition(instrument.id, position)) {
      position.instrument_id = instrument.id;
    }
    if (!storage_->Query(
            "INSERT INTO account_positions (ledger_id,"
            "instrument_id,total_qty,available_qty,reserved_qty,"
            "unavailable_qty,source_run_id,source_lsn) "
            "VALUES (?,?,?,?,?,?,?,?)",
            {ledger, Value(position.instrument_id.value),
             Value(position.total_qty), Value(position.available_qty),
             Value(position.reserved_qty),
             Value(position.total_qty - position.available_qty -
                   position.reserved_qty),
             run, sequence}))
      return DatabaseFailure();
  }
  return {};
#else
  (void)projection;
  return DatabaseFailure();
#endif
}

base::Status MysqlArchiveStore::FinalizeRun(const ArchiveProjection& projection,
                                            std::uint64_t final_lsn) noexcept {
#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
  if (storage_ == nullptr || !storage_->transaction ||
      projection.run_id() != storage_->pending_run ||
      projection.last_lsn() != final_lsn || final_lsn != storage_->pending_lsn)
    return Fail(base::ErrorCode::kInvalidState,
                "archive final boundary mismatch");
  if (!storage_->Exact(
          "SELECT final_lsn IS NULL OR final_lsn=? FROM archive_runs "
          "WHERE ledger_id=? AND run_id=?",
          {Value(final_lsn), Value(storage_->ledger),
           Value(projection.run_id())}) ||
      !storage_->Query(
          "UPDATE archive_runs SET final_lsn=? WHERE ledger_id=? AND run_id=? "
          "AND final_lsn IS NULL",
          {Value(final_lsn), Value(storage_->ledger),
           Value(projection.run_id())}))
    return DatabaseFailure();
  return {};
#else
  (void)projection;
  (void)final_lsn;
  return DatabaseFailure();
#endif
}

base::Status MysqlArchiveStore::Commit(
    const JournalCursor& cursor, const ArchiveProjection& projection) noexcept {
#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
  if (storage_ == nullptr || !storage_->transaction ||
      cursor.stream_id != storage_->pending_run ||
      cursor.lsn != storage_->pending_lsn ||
      projection.run_id() != cursor.stream_id ||
      projection.last_lsn() != cursor.lsn)
    return Fail(base::ErrorCode::kInvalidState,
                "archive commit cursor mismatch");
  auto status = WriteProjection(projection);
  if (!status.ok()) return status;
  if (!storage_->Query(
          "UPDATE archive_progress SET run_id=?,"
          "baseline_installed=1,lsn=?,segment_no=?,segment_offset=?,"
          "projection_version=1 WHERE ledger_id=?",
          {Value(cursor.stream_id), Value(cursor.lsn), Value(cursor.segment),
           Value(cursor.offset), Value(storage_->ledger)}))
    return DatabaseFailure();
  if (!storage_->Raw("COMMIT")) {
    // The server may have committed before the reply was lost. Never infer a
    // rollback from this failure or advance an in-memory acknowledgment.
    const auto result = Fail(base::ErrorCode::kIoError, storage_->detail, true);
    Close();
    return result;
  }
  storage_->transaction = false;
  storage_->progress = {true, true, cursor};
  return {};
#else
  (void)cursor;
  (void)projection;
  return DatabaseFailure();
#endif
}

void MysqlArchiveStore::Rollback() noexcept {
#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
  if (storage_ != nullptr && storage_->transaction) {
    // Preserve the causal diagnostic. An unsuccessful rollback discards the
    // connection so the next attempt cannot reuse an uncertain transaction.
    if (mysql_rollback(storage_->connection) != 0) {
      Close();
      return;
    }
    storage_->transaction = false;
  }
#endif
}

}  // namespace yquant::tradingd
