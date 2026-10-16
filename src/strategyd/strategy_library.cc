#include "strategyd/strategy_library.h"

#include <dlfcn.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace yquant::strategyd {

StrategyLibrary::~StrategyLibrary() noexcept { (void)Close(); }

void StrategyLibrary::SetError(const char* stage, const char* detail) noexcept {
  if (error_stage_[0] != '\0') {
    return;
  }
  error_stage_ = stage;
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
}

base::Status StrategyLibrary::Open(const char* path) noexcept {
  error_stage_ = "";
  error_detail_[0] = '\0';
  if (handle_ != nullptr) {
    SetError("command", "a strategy library is already open");
    return {base::ErrorCode::kInvalidState, {}};
  }
  if (path == nullptr || path[0] == '\0') {
    SetError("path", "strategy library path must not be empty");
    return {base::ErrorCode::kInvalidArgument, {}};
  }

  // realpath allocates only during loading and prevents bare filenames from
  // being interpreted as dynamic-loader search names.
  char* absolute_path = realpath(path, nullptr);
  if (absolute_path == nullptr) {
    const int saved_errno = errno;
    SetError("path", "cannot resolve strategy library path");
    const auto code = saved_errno == ENOMEM
                          ? base::ErrorCode::kResourceExhausted
                          : base::ErrorCode::kIoError;
    return {code, {saved_errno, std::generic_category()}};
  }

  handle_ = dlopen(absolute_path, RTLD_NOW | RTLD_LOCAL);
  if (handle_ == nullptr) {
    // dlerror storage may change on the next loader operation.
    const char* error = dlerror();
    SetError("dlopen",
             error != nullptr ? error : "dynamic library load failed");
    std::free(absolute_path);
    return {base::ErrorCode::kIoError, {}};
  }
  std::free(absolute_path);

  (void)dlerror();
  void* symbol = dlsym(handle_, "YQuantGetStrategyPlugin");
  const char* error = dlerror();
  if (error != nullptr || symbol == nullptr) {
    SetError("dlsym",
             error != nullptr ? error : "strategy entry point is null");
    (void)Close();
    return {base::ErrorCode::kCorrupt, {}};
  }

  const auto get_plugin = reinterpret_cast<strategy::GetStrategyPlugin>(symbol);
  plugin_ = get_plugin();
  base::Status result;
  if (plugin_ == nullptr) {
    SetError("descriptor", "strategy plugin descriptor is null");
    result.code = base::ErrorCode::kCorrupt;
  } else if (plugin_->abi_version != strategy::kStrategyPluginAbiVersion) {
    SetError("descriptor", "unsupported strategy plugin ABI version");
    result.code = base::ErrorCode::kUnsupported;
  } else if (plugin_->struct_size != sizeof(strategy::StrategyPlugin)) {
    SetError("descriptor", "unsupported strategy plugin descriptor size");
    result.code = base::ErrorCode::kUnsupported;
  } else if (plugin_->create == nullptr || plugin_->destroy == nullptr) {
    SetError("descriptor",
             "strategy plugin factory functions must not be null");
    result.code = base::ErrorCode::kCorrupt;
  }
  if (!result.ok()) {
    (void)Close();
  }
  return result;
}

base::Status StrategyLibrary::Close() noexcept {
  // No descriptor or function pointer may outlive the dlopen reference.
  plugin_ = nullptr;
  void* handle = handle_;
  handle_ = nullptr;
  if (handle == nullptr) {
    return {};
  }
  if (dlclose(handle) != 0) {
    const char* error = dlerror();
    SetError("dlclose",
             error != nullptr ? error : "dynamic library close failed");
    return {base::ErrorCode::kIoError, {}};
  }
  return {};
}

}  // namespace yquant::strategyd
