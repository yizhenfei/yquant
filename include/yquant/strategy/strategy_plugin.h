#ifndef YQUANT_STRATEGY_STRATEGY_PLUGIN_H_
#define YQUANT_STRATEGY_STRATEGY_PLUGIN_H_

#include <cstdint>

#include "yquant/strategy/strategy.h"

#define YQUANT_STRATEGY_EXPORT __attribute__((visibility("default")))

namespace yquant::strategy {

inline constexpr std::uint32_t kStrategyPluginAbiVersion = 5;

// Descriptor and function pointers remain valid while the library is open.
// Host and plugin must use compatible C++ ABIs and the same interface version.
struct StrategyPlugin {
  std::uint32_t abi_version;
  std::uint32_t struct_size;

  // out is a required output slot; its stored pointer may be null.
  // Sets out to a destroyable instance on success and nullptr on failure.
  // Any nonnull output, including on error, must be recoverable with destroy().
  base::ErrorCode (*create)(Strategy*& out) noexcept;
  void (*destroy)(Strategy* instance) noexcept;
};

using GetStrategyPlugin = const StrategyPlugin* (*)() noexcept;

}  // namespace yquant::strategy

// Return an immutable descriptor; do not create or start the strategy here.
extern "C" YQUANT_STRATEGY_EXPORT const yquant::strategy::StrategyPlugin*
YQuantGetStrategyPlugin() noexcept;

#endif  // YQUANT_STRATEGY_STRATEGY_PLUGIN_H_
