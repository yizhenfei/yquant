#ifndef YQUANT_MD_INSTRUMENT_CATALOG_H_
#define YQUANT_MD_INSTRUMENT_CATALOG_H_

#include <stddef.h>

#include <memory>
#include <string_view>

#include "yquant/base/config_error.h"
#include "yquant/base/status.h"
#include "yquant/model/instrument.h"

namespace yquant::md {

// Catalog capacity is independent of a process's active instrument limit.
inline constexpr size_t kMaxInstrumentCatalogEntries = 16384;

// Validates identity and all equity/futures contract units. Failure owns a
// field diagnostic and leaves definition unchanged; path is call-only.
[[nodiscard]] base::Status ValidateInstrumentDefinition(
    const model::InstrumentDefinition& definition, std::string_view path,
    base::ConfigError& error) noexcept;

// Owns a sorted, bounded dictionary. Load performs initialization I/O and
// allocation; Find copies without I/O or allocation. Serialize all access.
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
  size_t size() const noexcept { return size_; }
  const base::ConfigError& error() const noexcept { return error_; }

 private:
  std::unique_ptr<model::InstrumentDefinition[]> definitions_;
  size_t size_ = 0;
  base::ConfigError error_;
};

}  // namespace yquant::md

#endif  // YQUANT_MD_INSTRUMENT_CATALOG_H_
