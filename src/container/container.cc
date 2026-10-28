#include "yquant/container/container.h"

#include <algorithm>
#include <cstddef>

namespace yquant::container {

bool Container::Register(Pollable& component) {
  if (running_ ||
      std::find(components_.begin(), components_.end(), &component) !=
          components_.end()) {
    return false;
  }
  components_.push_back(&component);
  return true;
}

bool Container::Run() {
  if (running_) {
    return false;
  }
  running_ = true;
  std::size_t remaining = components_.size();
  while (remaining != 0) {
    for (Pollable*& component : components_) {
      if (component != nullptr && component->Poll() == PollResult::kFinished) {
        component = nullptr;
        --remaining;
      }
    }
  }
  components_.clear();
  running_ = false;
  return true;
}

}  // namespace yquant::container
