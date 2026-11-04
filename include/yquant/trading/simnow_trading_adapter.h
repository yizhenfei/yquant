#ifndef YQUANT_TRADING_SIMNOW_TRADING_ADAPTER_H_
#define YQUANT_TRADING_SIMNOW_TRADING_ADAPTER_H_

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "yquant/trading/simnow_startup_snapshot.h"
#include "yquant/trading/trading_adapter.h"

namespace yquant::trading {

// Connect copies the supplied values. system_info holds the binary terminal
// data for a macOS front that requires it; leave it empty on Linux.
struct SimNowTraderConfig {
  std::string_view front, broker, user, password, app_id, auth_code;
  std::string_view system_info{};
  std::chrono::milliseconds timeout{30000};
  SimNowInstruments instruments{};
  // Trading sessions can confirm daily settlement before startup queries.
  bool confirm_settlement = false;
};

// Identifies an order in a subsequent account query for the same trading day.
struct SimNowNativeOrderKey {
  int front, session;
  std::string ref, exchange, system_id;
};

enum class LoginPhase {
  kConnecting, kAuthenticating, kLoggingIn, kLoggedIn, kDisconnected, kFailed
};

// Business-thread view. Session identifiers are populated only while logged
// in. reason names the failed operation and code carries the SDK error number.
struct TraderLoginStatus {
  LoginPhase phase = LoginPhase::kConnecting;
  std::string trading_day, max_order_ref;
  int front_id = 0, session_id = 0, code = 0;
  const char* reason = "";
};

// Connects one SimNow trading account. Drive Run on the business thread until
// login succeeds or fails. Login alone does not supply an account baseline.
class SimNowTradingAdapter : public ITradingAdapter {
 public:
  // Returns nullptr for invalid configuration or failure to create the SDK.
  static std::unique_ptr<SimNowTradingAdapter> Connect(
      const SimNowTraderConfig& config);
  // Borrowed until the next Run or destruction. Read on the business thread.
  virtual const TraderLoginStatus& login_status() const = 0;
  // Starts read-only queries after login. Retry after a failed query or an
  // unresolved hold. A verified snapshot needs no further collection.
  // A nonempty symbol also requests that futures contract's speculative
  // margin and fees. The adapter copies the symbol before returning.
  virtual bool StartSnapshot(std::string_view symbol = {}) = 0;
  // Drive Run to advance the queries. Both accessors use the business thread.
  virtual SnapshotStatus snapshot_status() const = 0;
  // Copies the native identity bound to an order submitted by this session.
  // Before exchange acceptance system_id can be empty.
  virtual std::optional<SimNowNativeOrderKey> NativeKey(OrderId order) const = 0;
  // Returns a borrowed raw snapshot once collected. Check holds_error before
  // using holds. A retry or a processed disconnect invalidates the snapshot.
  virtual const SimNowStartupSnapshot* snapshot() const = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_SIMNOW_TRADING_ADAPTER_H_
