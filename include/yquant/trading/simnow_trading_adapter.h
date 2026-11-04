#ifndef YQUANT_TRADING_SIMNOW_TRADING_ADAPTER_H_
#define YQUANT_TRADING_SIMNOW_TRADING_ADAPTER_H_

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "yquant/trading/simnow_startup_snapshot.h"
#include "yquant/trading/trading_adapter.h"

namespace yquant::trading {

// Configures a SimNow trading connection and its instrument mappings.
// Connect copies these settings. system_info contains binary terminal data
// required by some macOS fronts; leave it empty on Linux.
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

// Reports login progress and session identifiers from a successful login.
// reason names a failed operation and code carries the SDK error number.
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
  // Returns the login status, which Run may update.
  virtual const TraderLoginStatus& login_status() const = 0;
  // Starts read-only queries after login and returns true. A nonempty symbol
  // also requests that futures contract's speculative margin and fees.
  // Returns false for an invalid symbol, an unfinished login, an active query
  // or a completed snapshot without hold errors. Failed queries and snapshots
  // with hold errors may be retried. Drive Run to advance accepted queries.
  virtual bool StartSnapshot(std::string_view symbol = {}) = 0;
  virtual SnapshotStatus snapshot_status() const = 0;
  // Returns the native identity of an order submitted by this session, or
  // nullopt if unknown. system_id may be empty before exchange acceptance.
  virtual std::optional<SimNowNativeOrderKey> NativeKey(OrderId order) const = 0;
  // Returns nullptr until collection completes. Check holds_error before using
  // the per-order holds. A retry or processed disconnect invalidates the result.
  virtual const SimNowStartupSnapshot* snapshot() const = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_SIMNOW_TRADING_ADAPTER_H_
