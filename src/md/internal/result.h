#ifndef YQUANT_MD_INTERNAL_RESULT_H_
#define YQUANT_MD_INTERNAL_RESULT_H_

#include <optional>
#include <utility>

#include "yquant/base/status.h"

namespace yquant::md::internal {

// Internal, allocation-free result used to assemble resources before committing
// them to the public object. A failed result has no value.
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}
  Result(base::Status error) : error_(error) {}
  explicit operator bool() const noexcept { return value_.has_value(); }
  T& operator*() { return *value_; }
  const T& operator*() const { return *value_; }
  T* operator->() { return &*value_; }
  const T* operator->() const { return &*value_; }
  base::Status error() const noexcept { return error_; }

 private:
  std::optional<T> value_;
  base::Status error_;
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(base::Status error) : error_(error) {}
  explicit operator bool() const noexcept { return error_.ok(); }
  base::Status error() const noexcept { return error_; }

 private:
  base::Status error_;
};

}  // namespace yquant::md::internal

#endif  // YQUANT_MD_INTERNAL_RESULT_H_
