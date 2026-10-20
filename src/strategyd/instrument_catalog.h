#ifndef YQUANT_STRATEGYD_INSTRUMENT_CATALOG_H_
#define YQUANT_STRATEGYD_INSTRUMENT_CATALOG_H_

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

#include "yquant/base/status.h"
#include "yquant/model/instrument.h"
#include "yquant/strategy/strategy_config.h"

namespace yquant::strategyd {

// Catalog capacity is independent of a process's active instrument limit.
inline constexpr std::size_t kMaxInstrumentCatalogEntries = 16384;
// Maximum resolved path bytes, excluding the terminating NUL.
inline constexpr std::size_t kMaxInstrumentCatalogPathLength = 4095;

// Owns a sorted, bounded instrument dictionary shared through a static file.
// Load performs file I/O and allocation during initialization; Find copies a
// definition without I/O or allocation. Serialize Load and all queries.
class InstrumentCatalogLoader {
 public:
  InstrumentCatalogLoader() noexcept = default;
  InstrumentCatalogLoader(const InstrumentCatalogLoader&) = delete;
  InstrumentCatalogLoader& operator=(const InstrumentCatalogLoader&) = delete;

  // Replaces the catalog from strict JSON. Failure leaves size() zero and no
  // queryable definitions; error() owns diagnostics until the next Load.
  // Borrows path only during this call and retains no JSON/parser views.
  [[nodiscard]] base::Status Load(const char* path) noexcept;

  // Copies a matching definition to out. Failure preserves out.
  [[nodiscard]] bool Find(model::InstrumentId id,
                          model::InstrumentDefinition& out) const noexcept;
  std::size_t size() const noexcept { return size_; }
  const strategy::ConfigError& error() const noexcept { return error_; }

 private:
  std::unique_ptr<model::InstrumentDefinition[]> definitions_;
  std::size_t size_ = 0;
  strategy::ConfigError error_;
};

// Resolves reference relative to config_path's directory and writes an absolute
// NUL-terminated path to out. Empty, embedded-NUL or overlong references and
// insufficient output space return kInvalidArgument; filesystem errors return
// kIoError. Failure preserves out and fills error; success clears error.
// Inputs and output storage are borrowed only during this initialization call.
[[nodiscard]] base::Status ResolveInstrumentCatalogPath(
    const char* config_path, std::string_view reference, std::span<char> out,
    strategy::ConfigError& error) noexcept;

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_INSTRUMENT_CATALOG_H_
