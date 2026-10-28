#ifndef YQUANT_CONTAINER_CONTAINER_H_
#define YQUANT_CONTAINER_CONTAINER_H_

#include <functional>
#include <vector>

#include "yquant/container/service.h"

namespace yquant::container {

// Repeatedly calls registered services until the exit condition is met.
// Register grows the registration list as needed; Run reuses its storage.
// Services are not owned. Keep them alive until Run returns.
// A completed run removes all registrations.
class Container {
 public:
  Container() = default;
  Container(const Container&) = delete;
  Container& operator=(const Container&) = delete;

  // Registers a service for the next Run. Returns false if it is already
  // registered or Run is active.
  bool Register(Service& service);
  // Calls each service's Run in registration order on the calling thread.
  // Checks should_stop before each call. An empty condition runs indefinitely
  // when services are registered. With no services, returns true immediately.
  // Returns true when the condition is true and clears all registrations.
  // Returns false if Run is already active, without checking the condition.
  // Uses the condition only during this call; it must not block.
  bool Run(const std::function<bool()>& should_stop = {});

 private:
  std::vector<Service*> services_;
  bool running_ = false;
};

}  // namespace yquant::container

#endif  // YQUANT_CONTAINER_CONTAINER_H_
