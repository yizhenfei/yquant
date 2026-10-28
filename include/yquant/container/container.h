#ifndef YQUANT_CONTAINER_CONTAINER_H_
#define YQUANT_CONTAINER_CONTAINER_H_

#include <vector>

namespace yquant::container {

// Tells the container whether to poll this component again.
enum class PollResult { kContinue, kFinished };

// Implements one step of work whenever the container calls Poll.
class Pollable {
 public:
  virtual ~Pollable() = default;
  // Performs one step. Returns kContinue to run again in the next round,
  // or kFinished when this component has completed all its work.
  virtual PollResult Poll() = 0;
};

// Runs registered components in rounds until every component finishes.
// Register grows the registration list as needed; Run reuses its storage.
// Components are not owned. Keep them alive until Run returns.
// A completed run removes all registrations.
class Container {
 public:
  Container() = default;
  Container(const Container&) = delete;
  Container& operator=(const Container&) = delete;

  // Registers a component for the next Run. Returns false if it is already
  // registered or Run is active.
  bool Register(Pollable& component);
  // Polls components in registration order, skipping those that have finished.
  // Returns true when all finish, including when none were registered.
  // Returns false if called from inside an active Run.
  bool Run();

 private:
  std::vector<Pollable*> components_;
  bool running_ = false;
};

}  // namespace yquant::container

#endif  // YQUANT_CONTAINER_CONTAINER_H_
