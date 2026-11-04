#ifndef YQUANT_CONTAINER_CONTAINER_H_
#define YQUANT_CONTAINER_CONTAINER_H_

#include <functional>
#include <vector>

#include "yquant/container/service.h"

namespace yquant::container {

// Repeatedly runs services in registration order on the calling thread.
// Container borrows each service. Keep it alive while it is registered.
// Run clears all registrations before returning successfully.
// Different threads must not call methods on the same Container concurrently.
class Container {
 public:
  Container() = default;
  Container(const Container&) = delete;
  Container& operator=(const Container&) = delete;

  // Registers service for the next Run and returns true. Returns false without
  // changing the list if service is already registered or if a running service
  // or stop condition calls Register on this container.
  bool Register(Service& service);
  // Returns true when scheduling finishes and all registrations are cleared.
  // Returns false if Run is already active on this container. The nested call
  // leaves the outer run unchanged and does not invoke its own stop condition.
  //
  // An empty service list completes immediately without checking should_stop.
  // Otherwise, Run checks should_stop before each service call and stops when
  // should_stop returns true. With no condition, Run loops indefinitely.
  // Run does not retain the condition; the condition must not block.
  bool Run(const std::function<bool()>& should_stop = {});

 private:
  std::vector<Service*> services_;
  bool running_ = false;
};

}  // namespace yquant::container

#endif  // YQUANT_CONTAINER_CONTAINER_H_
