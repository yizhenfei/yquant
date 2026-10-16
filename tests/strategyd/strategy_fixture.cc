#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <new>

#include <unistd.h>

#include "yquant/strategy/strategy.h"
#include "yquant/strategy/strategy_plugin.h"

// Each target selects a distinct plugin failure, exercising the real loader.
#ifndef YQUANT_TEST_PLUGIN_MODE
#define YQUANT_TEST_PLUGIN_MODE 0
#endif

extern "C" int YQuantTestDependency() noexcept;

namespace {

using yquant::base::ErrorCode;
using yquant::strategy::Strategy;
using yquant::strategy::StrategyPlugin;

void Emit(char event) noexcept {
  const char* value = std::getenv("YQUANT_TEST_EVENT_FD");
  if (value == nullptr) {
    return;
  }
  const int fd = static_cast<int>(std::strtol(value, nullptr, 10));
  while (write(fd, &event, 1) == -1 && errno == EINTR) {
  }
}

struct ModuleLifetime {
  ~ModuleLifetime() { Emit('U'); }
};

ModuleLifetime module_lifetime;

class FixtureStrategy final : public Strategy {
 public:
  ErrorCode OnStart() noexcept override {
    Emit('S');
    if constexpr (YQUANT_TEST_PLUGIN_MODE == 12) {
      if (YQuantTestDependency() != 1) {
        return ErrorCode::kInternalError;
      }
    }
    const char* value = std::getenv("YQUANT_TEST_START_RELEASE_FD");
    if (value != nullptr) {
      const int fd = static_cast<int>(std::strtol(value, nullptr, 10));
      char release = 0;
      while (read(fd, &release, 1) == -1 && errno == EINTR) {
      }
    }
    if constexpr (YQUANT_TEST_PLUGIN_MODE == 9) {
      return ErrorCode::kInvalidArgument;
    }
    return ErrorCode::kOk;
  }

  ErrorCode OnStop() noexcept override {
    Emit('T');
    if constexpr (YQUANT_TEST_PLUGIN_MODE == 10) {
      return ErrorCode::kInvalidArgument;
    }
    return ErrorCode::kOk;
  }
};

ErrorCode Create(Strategy** strategy) noexcept {
  Emit('C');
  if constexpr (YQUANT_TEST_PLUGIN_MODE == 6) {
    *strategy = nullptr;
    return ErrorCode::kInvalidArgument;
  }
  if constexpr (YQUANT_TEST_PLUGIN_MODE == 7) {
    *strategy = nullptr;
    return ErrorCode::kOk;
  }
  *strategy = new (std::nothrow) FixtureStrategy;
  if constexpr (YQUANT_TEST_PLUGIN_MODE == 8) {
    return ErrorCode::kInvalidArgument;
  }
  return *strategy != nullptr ? ErrorCode::kOk
                              : ErrorCode::kResourceExhausted;
}

void Destroy(Strategy* strategy) noexcept {
  delete strategy;
  Emit('D');
}

[[maybe_unused]] const StrategyPlugin plugin = {
    YQUANT_TEST_PLUGIN_MODE == 2
        ? yquant::strategy::kStrategyPluginAbiVersion + 1
        : yquant::strategy::kStrategyPluginAbiVersion,
    YQUANT_TEST_PLUGIN_MODE == 3
        ? 0U
        : static_cast<std::uint32_t>(sizeof(StrategyPlugin)),
    YQUANT_TEST_PLUGIN_MODE == 4 ? nullptr : &Create,
    YQUANT_TEST_PLUGIN_MODE == 5 ? nullptr : &Destroy,
};

}  // namespace

#if YQUANT_TEST_PLUGIN_MODE != 1
extern "C" YQUANT_STRATEGY_EXPORT const StrategyPlugin*
YQuantGetStrategyPlugin() noexcept {
  if constexpr (YQUANT_TEST_PLUGIN_MODE == 11) {
    return nullptr;
  }
  return &plugin;
}
#endif
