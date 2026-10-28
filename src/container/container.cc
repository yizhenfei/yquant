#include "yquant/container/container.h"

#include <algorithm>
#include <functional>

namespace yquant::container {

bool Container::Register(Service& service) {
  if (running_ ||
      std::find(services_.begin(), services_.end(), &service) !=
          services_.end()) {
    return false;
  }
  services_.push_back(&service);
  return true;
}

bool Container::Run(const std::function<bool()>& should_stop) {
  if (running_) {
    return false;
  }
  running_ = true;
  bool stopped = false;
  while (!services_.empty() && !stopped) {
    for (Service* service : services_) {
      if (should_stop && should_stop()) {
        stopped = true;
        break;
      }
      service->Run();
    }
  }
  services_.clear();
  running_ = false;
  return true;
}

}  // namespace yquant::container
