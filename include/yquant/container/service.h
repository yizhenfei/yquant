#ifndef YQUANT_CONTAINER_SERVICE_H_
#define YQUANT_CONTAINER_SERVICE_H_

namespace yquant::container {

// Provides the entry point that the container calls repeatedly on its thread.
class Service {
 public:
  virtual ~Service() = default;
  // Processes currently available work and returns promptly so other services
  // can run. The container calls Run again until its exit condition is met.
  virtual void Run() = 0;
};

}  // namespace yquant::container

#endif  // YQUANT_CONTAINER_SERVICE_H_
