#include "xenon/kernel/wait.hpp"

#include "xenon/kernel/event.hpp"
#include "xenon/kernel/mutant.hpp"
#include "xenon/kernel/semaphore.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/kernel/timer.hpp"

namespace xenon::kernel {

bool is_waitable_object(ObjectType type) {
  switch (type) {
    case ObjectType::Event:
    case ObjectType::Thread:
    case ObjectType::Semaphore:
    case ObjectType::Mutant:
    case ObjectType::Timer:
      return true;
    default:
      return false;
  }
}

bool wait_on_object(KernelObject& object, std::chrono::milliseconds timeout,
                    std::uint32_t waiting_thread_id) {
  switch (object.type()) {
    case ObjectType::Event:
      return static_cast<KernelEvent&>(object).wait_for(timeout);
    case ObjectType::Semaphore:
      return static_cast<KernelSemaphore&>(object).wait_for(timeout);
    case ObjectType::Timer:
      return static_cast<KernelTimer&>(object).wait_for(timeout);
    case ObjectType::Mutant:
      return static_cast<KernelMutant&>(object).acquire(waiting_thread_id, timeout);
    case ObjectType::Thread: {
      // Waiting on a thread means waiting for it to terminate
      auto& thread = static_cast<KernelThread&>(object);
      return thread.state() == ThreadState::Terminated || 
             thread.join(static_cast<std::uint32_t>(timeout.count()));
    }
    default:
      return false;
  }
}

WaitResult wait_for_single_object(
    const std::shared_ptr<KernelObject>& object,
    std::chrono::milliseconds timeout,
    std::uint32_t waiting_thread_id) {
  if (!object || !is_waitable_object(object->type())) {
    return WaitResult::Failed;
  }

  if (wait_on_object(*object, timeout, waiting_thread_id)) {
    return WaitResult::Success;
  }

  return WaitResult::Timeout;
}

WaitResult wait_for_multiple_objects(
    std::span<const std::shared_ptr<KernelObject>> objects,
    bool wait_all,
    std::chrono::milliseconds timeout,
    std::uint32_t* signaled_index,
    std::uint32_t waiting_thread_id) {

  if (objects.empty() || objects.size() > 64) {
    return WaitResult::Failed;
  }

  // Verify all objects are waitable
  for (const auto& obj : objects) {
    if (!obj || !is_waitable_object(obj->type())) {
      return WaitResult::Failed;
    }
  }

  auto start_time = std::chrono::steady_clock::now();
  auto remaining_timeout = timeout;

  if (wait_all) {
    // Wait for all objects
    for (std::size_t i = 0; i < objects.size(); ++i) {
      if (timeout.count() != 0xFFFFFFFF && timeout.count() != -1) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        remaining_timeout = timeout - elapsed;
        if (remaining_timeout.count() <= 0) {
          return WaitResult::Timeout;
        }
      }

      if (!wait_on_object(*objects[i], remaining_timeout, waiting_thread_id)) {
        return WaitResult::Timeout;
      }
    }
    return WaitResult::Success;
  } else {
    // Wait for any object
    // Simple polling implementation - a full implementation would use condition variables
    while (true) {
      for (std::size_t i = 0; i < objects.size(); ++i) {
        if (wait_on_object(*objects[i], std::chrono::milliseconds(0), waiting_thread_id)) {
          if (signaled_index) {
            *signaled_index = static_cast<std::uint32_t>(i);
          }
          return WaitResult::Success;
        }
      }

      if (timeout.count() != 0xFFFFFFFF && timeout.count() != -1) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        if (elapsed >= timeout) {
          return WaitResult::Timeout;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }
}

}  // namespace xenon::kernel
