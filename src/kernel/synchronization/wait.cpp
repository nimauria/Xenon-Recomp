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
#include <optional>
#include <thread>


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

  static MultiWaitList& wait_list(KernelObject& object) {
    switch (object.type()) {
      case ObjectType::Event: return static_cast<KernelEvent&>(object).multi_waiters_;
      case ObjectType::Semaphore: return static_cast<KernelSemaphore&>(object).multi_waiters_;
      case ObjectType::Mutant: return static_cast<KernelMutant&>(object).multi_waiters_;
      case ObjectType::Timer: return static_cast<KernelTimer&>(object).multi_waiters_;
      default: return static_cast<KernelThread&>(object).multi_waiters_;
    }
  }

  // Returns whether the wait took over an abandoned mutant.
  static bool satisfy(KernelObject& object, std::uint32_t thread_id) {
    switch (object.type()) {
      case ObjectType::Event: static_cast<KernelEvent&>(object).satisfy_locked(); break;
      case ObjectType::Semaphore: static_cast<KernelSemaphore&>(object).satisfy_locked(); break;
      case ObjectType::Mutant:
        return static_cast<KernelMutant&>(object).satisfy_locked(thread_id);
      case ObjectType::Timer: static_cast<KernelTimer&>(object).satisfy_locked(); break;
      default: break;
    }
    return false;
  }
};

}  // namespace detail

namespace {

constexpr std::size_t kMaxWaitObjects = 64;

}  // namespace

void detail::MultiWaitList::add(MultiWaitBlock* block) {
  std::scoped_lock lock(mutex_);
  blocks_.push_back(block);
  size_.store(static_cast<std::uint32_t>(blocks_.size()), std::memory_order_seq_cst);
}

void detail::MultiWaitList::remove(MultiWaitBlock* block) noexcept {
  std::scoped_lock lock(mutex_);
  if (const auto found = std::find(blocks_.begin(), blocks_.end(), block); found != blocks_.end()) {
    *found = blocks_.back();
    blocks_.pop_back();
  }
  size_.store(static_cast<std::uint32_t>(blocks_.size()), std::memory_order_seq_cst);
}

void detail::MultiWaitList::notify_always() noexcept {
  std::scoped_lock lock(mutex_);
  for (auto* block : blocks_) {
    {
      std::scoped_lock block_lock(block->mutex);
      ++block->wakeups;
    }
    block->condition.notify_one();
  }
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
    std::uint32_t waiting_thread_id,
    bool alertable) {
  if (!object || !is_waitable_object(object->type())) {
    return WaitResult::Failed;
  }

  // A KernelThread's wait must end when the thread is terminated, which only
  // the multi-object path (registered on the waiter's own thread) provides.
  // Waiting for another thread to finish still goes through join(), which
  // terminate() cannot interrupt.
  if (KernelThread::current() && object->type() != ObjectType::Thread) {
    std::uint32_t index = 0;
    return wait_for_multiple_objects(std::span(&object, 1), false, timeout, &index,
                                     waiting_thread_id, alertable);
  }

  if (object->type() == ObjectType::Mutant) {
    bool abandoned = false;
    if (!static_cast<KernelMutant&>(*object).acquire(waiting_thread_id, timeout, &abandoned)) {
      return WaitResult::Timeout;
    }
    return abandoned ? WaitResult::Abandoned : WaitResult::Success;
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
    std::uint32_t waiting_thread_id,
    bool alertable) {

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
  // lowest-indexed satisfiable object. Taking over an abandoned mutant makes
  // the result Abandoned. As in the NT wait code the Xbox kernel derives
  // from, a WaitAny reports the satisfying index with it and a WaitAll
  // reports index 0 (STATUS_ABANDONED) whichever mutant was abandoned.
  bool abandoned = false;
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
      for (auto* object : distinct) {
        if (detail::WaitAccess::satisfy(*object, waiting_thread_id)) abandoned = true;
      }
      if (abandoned && signaled_index) *signaled_index = 0;
      return true;
    }
    for (std::size_t i = 0; i < objects.size(); ++i) {
      if (detail::WaitAccess::can_satisfy(*objects[i], waiting_thread_id)) {
        abandoned = detail::WaitAccess::satisfy(*objects[i], waiting_thread_id);
        if (signaled_index) *signaled_index = static_cast<std::uint32_t>(i);
        return true;
      }
    }
    return false;
  };
  const auto satisfied = [&] { return abandoned ? WaitResult::Abandoned : WaitResult::Success; };

  const bool infinite = timeout.count() < 0 || timeout.count() >= 0xFFFFFFFFll;
  const auto deadline =
      std::chrono::steady_clock::now() + (infinite ? std::chrono::milliseconds(0) : timeout);

  // A terminated thread takes nothing: its guest code is only finishing the
  // block it was in when terminate() was called.
  auto* self = KernelThread::current();
  const auto terminating = [self] { return self && self->is_terminated(); };
  const auto apc_pending = [self, alertable] {
    return alertable && self && self->user_apc_pending();
  };
  // As NT's TestForAlertPending: an alert is taken before a pending APC.
  const auto alert_or_apc = [self, alertable, &apc_pending]() -> std::optional<WaitResult> {
    if (alertable && self && self->take_alert()) return WaitResult::Alerted;
    if (apc_pending()) return WaitResult::UserApc;
    return std::nullopt;
  };
  if (terminating()) return WaitResult::Terminating;

  // A wait that is satisfied at once, or has a zero timeout, never registers.
  if (try_satisfy()) return satisfied();
  if (const auto ended = alert_or_apc()) return *ended;
  if (!infinite && timeout.count() == 0) return WaitResult::Timeout;

  // Registered on every object before the next check, so any change after
  // that check wakes this block (see multi_wait_list.hpp). A KernelThread
  // also registers on its own thread, so terminate(), a queued user APC and
  // an alert wake it.
  detail::MultiWaitBlock block;
  for (auto* object : distinct) detail::WaitAccess::wait_list(*object).add(&block);
  KernelObject* own_thread = nullptr;
  if (self && std::find(distinct.begin(), distinct.end(), self) == distinct.end()) {
    own_thread = self;
    detail::WaitAccess::wait_list(*own_thread).add(&block);
  }
  struct Unregister {
    std::span<KernelObject* const> objects;
    KernelObject* own_thread;
    detail::MultiWaitBlock* block;
    ~Unregister() {
      for (auto* object : objects) detail::WaitAccess::wait_list(*object).remove(block);
      if (own_thread) detail::WaitAccess::wait_list(*own_thread).remove(block);
    }
  } unregister{distinct, own_thread, &block};

  while (true) {
    std::uint64_t observed = 0;
    {
      std::scoped_lock lock(block.mutex);
      observed = block.wakeups;
    }
    if (terminating()) return WaitResult::Terminating;
    if (try_satisfy()) return satisfied();
    if (const auto ended = alert_or_apc()) return *ended;
    if (!infinite && std::chrono::steady_clock::now() >= deadline) return WaitResult::Timeout;

    std::unique_lock lock(block.mutex);
    const auto woken = [&] { return block.wakeups != observed; };
    if (infinite) {
      block.condition.wait(lock, woken);
    } else {
      // On timeout, loop once more: the final check above decides.
      (void)block.condition.wait_until(lock, deadline, woken);
    }
  }
}

WaitResult delay_current_thread(std::chrono::milliseconds delay, bool alertable) {
  auto* self = KernelThread::current();
  if (!self) {
    if (delay.count() > 0) std::this_thread::sleep_for(delay);
    return WaitResult::Success;
  }
  const auto apc_pending = [&] { return alertable && self->user_apc_pending(); };
  const auto alert_pending = [&] { return alertable && self->alerted(); };
  const auto ended = [&]() -> std::optional<WaitResult> {
    if (alertable && self->take_alert()) return WaitResult::Alerted;
    if (apc_pending()) return WaitResult::UserApc;
    return std::nullopt;
  };
  if (self->is_terminated()) return WaitResult::Terminating;
  if (const auto result = ended()) return *result;
  if (delay.count() <= 0) return WaitResult::Success;

  detail::MultiWaitBlock block;
  auto& own = detail::WaitAccess::wait_list(*self);
  own.add(&block);
  struct Unregister {
    detail::MultiWaitList& list;
    detail::MultiWaitBlock* block;
    ~Unregister() { list.remove(block); }
  } unregister{own, &block};
  const auto deadline = std::chrono::steady_clock::now() + delay;
  std::unique_lock lock(block.mutex);
  // Registered before this check, so a terminate() after it wakes the block.
  block.condition.wait_until(lock, deadline, [&] {
    return self->is_terminated() || apc_pending() || alert_pending();
  });
  if (self->is_terminated()) return WaitResult::Terminating;
  if (const auto result = ended()) return *result;
  return WaitResult::Success;
}

}  // namespace xenon::kernel
