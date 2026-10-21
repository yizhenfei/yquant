#ifndef YQUANT_MD_INSTRUMENT_CATALOG_H_
#define YQUANT_MD_INSTRUMENT_CATALOG_H_

#include <cstddef>
#include <memory>

#include "yquant/base/config_error.h"
#include "yquant/base/status.h"
#include "yquant/model/instrument.h"

namespace yquant::md {

// Catalog capacity is independent of a process's active instrument limit.
inline constexpr std::size_t kMaxInstrumentCatalogEntries = 16384;

// Owns a sorted, bounded instrument dictionary shared through a static file.
// Load performs file I/O and allocation during initialization; Find copies a
// definition without I/O or allocation. Serialize Load and all queries.
class InstrumentCatalog {
 public:
  InstrumentCatalog() noexcept = default;
  InstrumentCatalog(const InstrumentCatalog&) = delete;
  InstrumentCatalog& operator=(const InstrumentCatalog&) = delete;

  // Replaces the catalog from strict JSON. Failure leaves size() zero and no
  // queryable definitions; error() owns diagnostics until the next Load.
  // Borrows path only during this call and retains no JSON/parser views.
  [[nodiscard]] base::Status Load(const char* path) noexcept;

  // Copies a matching definition to out. Failure preserves out.
  [[nodiscard]] bool Find(model::InstrumentId id,
                          model::InstrumentDefinition& out) const noexcept;
  std::size_t size() const noexcept { return size_; }
  const base::ConfigError& error() const noexcept { return error_; }

 private:
  std::unique_ptr<model::InstrumentDefinition[]> definitions_;
  std::size_t size_ = 0;
  base::ConfigError error_;
};

}  // namespace yquant::md

#endif  // YQUANT_MD_INSTRUMENT_CATALOG_H_
