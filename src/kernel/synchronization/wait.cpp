#include "xenon/kernel/wait.hpp"

#include "xenon/kernel/event.hpp"
#include "xenon/kernel/mutant.hpp"
#include "xenon/kernel/semaphore.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/kernel/timer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>

#include "kernel/synchronization/wait_internal.hpp"

namespace xenon::kernel {

namespace detail {

// Reaches each waitable object's own mutex and its can-satisfy/satisfy rules.
// A thread has no mutex here: termination is final and read through
// is_terminated(), and waiting on a thread consumes nothing.
struct WaitAccess {
  static std::mutex* mutex(KernelObject& object) {
    switch (object.type()) {
      case ObjectType::Event: return &static_cast<KernelEvent&>(object).mutex_;
      case ObjectType::Semaphore: return &static_cast<KernelSemaphore&>(object).mutex_;
      case ObjectType::Mutant: return &static_cast<KernelMutant&>(object).mutex_;
      case ObjectType::Timer: return &static_cast<KernelTimer&>(object).mutex_;
      default: return nullptr;
    }
  }

  static bool can_satisfy(KernelObject& object, std::uint32_t thread_id) {
    switch (object.type()) {
      case ObjectType::Event: return static_cast<KernelEvent&>(object).can_satisfy_locked();
      case ObjectType::Semaphore: return static_cast<KernelSemaphore&>(object).can_satisfy_locked();
      case ObjectType::Mutant:
        return static_cast<KernelMutant&>(object).can_satisfy_locked(thread_id);
      case ObjectType::Timer: return static_cast<KernelTimer&>(object).can_satisfy_locked();
      case ObjectType::Thread: return static_cast<KernelThread&>(object).is_terminated();
      default: return false;
    }
  }

  static void satisfy(KernelObject& object, std::uint32_t thread_id) {
    switch (object.type()) {
      case ObjectType::Event: static_cast<KernelEvent&>(object).satisfy_locked(); break;
      case ObjectType::Semaphore: static_cast<KernelSemaphore&>(object).satisfy_locked(); break;
      case ObjectType::Mutant: static_cast<KernelMutant&>(object).satisfy_locked(thread_id); break;
      case ObjectType::Timer: static_cast<KernelTimer&>(object).satisfy_locked(); break;
      default: break;
    }
  }
};

}  // namespace detail

namespace {

constexpr std::size_t kMaxWaitObjects = 64;

struct MultiWaitNotifier {
  std::mutex mutex;
  std::condition_variable condition;
  std::uint64_t generation{0};
  std::atomic<std::uint32_t> waiters{0};
};

// Never destroyed: guest threads may still signal objects during process exit.
MultiWaitNotifier& multi_wait_notifier() {
  static auto* notifier = new MultiWaitNotifier();
  return *notifier;
}

void advance_generation(MultiWaitNotifier& notifier) {
  {
    std::scoped_lock lock(notifier.mutex);
    ++notifier.generation;
  }
  notifier.condition.notify_all();
}

}  // namespace

void detail::notify_multi_object_waiters() noexcept {
  auto& notifier = multi_wait_notifier();
  if (notifier.waiters.load(std::memory_order_seq_cst) != 0) advance_generation(notifier);
}

void detail::notify_multi_object_waiters_unconditionally() noexcept {
  advance_generation(multi_wait_notifier());
}

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

  if (objects.empty() || objects.size() > kMaxWaitObjects) {
    return WaitResult::Failed;
  }

  // Verify all objects are waitable
  for (const auto& obj : objects) {
    if (!obj || !is_waitable_object(obj->type())) {
      return WaitResult::Failed;
    }
  }

  // Each distinct object is locked once, in address order, so two waits over
  // overlapping sets cannot deadlock. A WaitAll naming the same object twice
  // needs it satisfiable once and consumes it once; what the Xbox kernel does
  // with duplicates has not been verified.
  std::array<KernelObject*, kMaxWaitObjects> lock_order{};
  for (std::size_t i = 0; i < objects.size(); ++i) lock_order[i] = objects[i].get();
  const auto lock_end = lock_order.begin() + static_cast<std::ptrdiff_t>(objects.size());
  std::sort(lock_order.begin(), lock_end, std::less<KernelObject*>{});
  const std::span<KernelObject* const> distinct(
      lock_order.begin(), std::unique(lock_order.begin(), lock_end));

  // Checks and consumes with every object's mutex held, so WaitAll takes all
  // of its objects or none of them, and WaitAny takes exactly one: the
  // lowest-indexed satisfiable object.
  const auto try_satisfy = [&]() -> bool {
    std::array<std::unique_lock<std::mutex>, kMaxWaitObjects> locks;
    std::size_t locked = 0;
    for (auto* object : distinct) {
      if (auto* mutex = detail::WaitAccess::mutex(*object)) {
        locks[locked++] = std::unique_lock(*mutex);
      }
    }
    if (wait_all) {
      for (auto* object : distinct) {
        if (!detail::WaitAccess::can_satisfy(*object, waiting_thread_id)) return false;
      }
      for (auto* object : distinct) detail::WaitAccess::satisfy(*object, waiting_thread_id);
      return true;
    }
    for (std::size_t i = 0; i < objects.size(); ++i) {
      if (detail::WaitAccess::can_satisfy(*objects[i], waiting_thread_id)) {
        detail::WaitAccess::satisfy(*objects[i], waiting_thread_id);
        if (signaled_index) *signaled_index = static_cast<std::uint32_t>(i);
        return true;
      }
    }
    return false;
  };

  const bool infinite = timeout.count() < 0 || timeout.count() >= 0xFFFFFFFFll;
  const auto deadline =
      std::chrono::steady_clock::now() + (infinite ? std::chrono::milliseconds(0) : timeout);

  // Registered before the first check, so any signal after that check
  // advances the generation this wait sleeps on (see wait_internal.hpp).
  auto& notifier = multi_wait_notifier();
  notifier.waiters.fetch_add(1, std::memory_order_seq_cst);
  struct Unregister {
    MultiWaitNotifier& notifier;
    ~Unregister() { notifier.waiters.fetch_sub(1, std::memory_order_seq_cst); }
  } unregister{notifier};

  while (true) {
    std::uint64_t observed = 0;
    {
      std::scoped_lock lock(notifier.mutex);
      observed = notifier.generation;
    }
    if (try_satisfy()) return WaitResult::Success;
    if (!infinite && std::chrono::steady_clock::now() >= deadline) return WaitResult::Timeout;

    std::unique_lock lock(notifier.mutex);
    const auto changed = [&] { return notifier.generation != observed; };
    if (infinite) {
      notifier.condition.wait(lock, changed);
    } else {
      // On timeout, loop once more: the final check above decides.
      (void)notifier.condition.wait_until(lock, deadline, changed);
    }
  }
}

}  // namespace xenon::kernel
