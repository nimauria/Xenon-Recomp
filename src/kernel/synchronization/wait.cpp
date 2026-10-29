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

std::mutex& dispatcher_mutex() noexcept {
  // Never destroyed: guest threads may still signal during process teardown.
  static auto* mutex = new std::mutex();
  return *mutex;
}

// Reaches each waitable object's can-satisfy/satisfy rules. Every call is
// made with the dispatcher lock held. A thread has no state under that lock:
// termination is final and read through is_terminated(), and waiting on a
// thread consumes nothing.
struct WaitAccess {
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
  static bool satisfy(KernelObject& object, std::uint32_t thread_id,
                      std::thread::id host_thread) {
    switch (object.type()) {
      case ObjectType::Event: static_cast<KernelEvent&>(object).satisfy_locked(); break;
      case ObjectType::Semaphore: static_cast<KernelSemaphore&>(object).satisfy_locked(); break;
      case ObjectType::Mutant:
        return static_cast<KernelMutant&>(object).satisfy_locked(thread_id, host_thread);
      case ObjectType::Timer: static_cast<KernelTimer&>(object).satisfy_locked(); break;
      default: break;
    }
    return false;
  }
};

namespace {

// Caller holds the dispatcher lock. Tests the wait and, when it can be
// satisfied, consumes what it takes: WaitAll takes all of its objects or
// none of them, and WaitAny takes exactly one, the lowest-indexed
// satisfiable object. Taking over an abandoned mutant sets `abandoned`. As in
// the NT wait code the Xbox kernel derives from, a WaitAny reports the
// satisfying index and a WaitAll reports index 0 (STATUS_ABANDONED)
// whichever mutant was abandoned. A WaitAll naming the same object twice
// needs it satisfiable once and consumes it once; what the Xbox kernel does
// with duplicates has not been verified.
bool try_satisfy_wait(std::span<KernelObject* const> objects,
                      std::span<KernelObject* const> distinct, bool wait_all,
                      std::uint32_t thread_id, std::thread::id host_thread, std::uint32_t& index,
                      bool& abandoned) {
  abandoned = false;
  if (wait_all) {
    for (auto* object : distinct) {
      if (!WaitAccess::can_satisfy(*object, thread_id)) return false;
    }
    for (auto* object : distinct) {
      if (WaitAccess::satisfy(*object, thread_id, host_thread)) abandoned = true;
    }
    index = 0;
    return true;
  }
  for (std::size_t i = 0; i < objects.size(); ++i) {
    if (WaitAccess::can_satisfy(*objects[i], thread_id)) {
      abandoned = WaitAccess::satisfy(*objects[i], thread_id, host_thread);
      index = static_cast<std::uint32_t>(i);
      return true;
    }
  }
  return false;
}

// Caller holds the dispatcher lock: records that `block`'s wait was
// satisfied, so no signaller satisfies it again, and wakes its waiter.
void grant(MultiWaitBlock& block, std::uint32_t index, bool abandoned) {
  {
    std::scoped_lock lock(block.mutex);
    block.granted = true;
    block.index = index;
    block.abandoned = abandoned;
    ++block.wakeups;
  }
  block.condition.notify_one();
}

}  // namespace

void MultiWaitList::add(MultiWaitBlock* block) {
  std::scoped_lock lock(mutex_);
  blocks_.push_back(block);
  size_.store(static_cast<std::uint32_t>(blocks_.size()), std::memory_order_relaxed);
}

void MultiWaitList::remove(MultiWaitBlock* block) noexcept {
  std::scoped_lock lock(mutex_);
  if (const auto found = std::find(blocks_.begin(), blocks_.end(), block); found != blocks_.end()) {
    blocks_.erase(found);
  }
  size_.store(static_cast<std::uint32_t>(blocks_.size()), std::memory_order_relaxed);
}

void MultiWaitList::satisfy_waiters_locked() noexcept {
  // Object lists are only changed with the dispatcher lock held, as here.
  if (size_.load(std::memory_order_relaxed) == 0) return;
  std::scoped_lock lock(mutex_);
  for (auto* block : blocks_) {
    if (block->granted || block->objects.empty()) continue;
    std::uint32_t index = 0;
    bool abandoned = false;
    if (try_satisfy_wait(block->objects, block->distinct, block->wait_all, block->thread_id,
                         block->host_thread, index, abandoned)) {
      grant(*block, index, abandoned);
    }
  }
}

void MultiWaitList::notify_always() noexcept {
  std::scoped_lock lock(mutex_);
  for (auto* block : blocks_) {
    {
      std::scoped_lock block_lock(block->mutex);
      ++block->wakeups;
    }
    block->condition.notify_one();
  }
}

}  // namespace detail

namespace {

constexpr std::size_t kMaxWaitObjects = 64;

// The wait under every wait. `signal`, when given, runs first, in the same
// dispatcher critical section that checks and registers the wait.
WaitResult wait_for_objects_after(const std::function<bool()>* signal,
                                  std::span<KernelObject* const> objects, bool wait_all,
                                  std::chrono::milliseconds timeout,
                                  std::uint32_t* signaled_index,
                                  std::uint32_t waiting_thread_id, bool alertable);

}  // namespace

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

std::size_t waiter_count(KernelObject& object) {
  if (!is_waitable_object(object.type())) return 0;
  return detail::WaitAccess::wait_list(object).size();
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

  // Waiting for another thread to finish goes through join(), which
  // terminate() cannot interrupt; every other wait is a registered wait,
  // which ends when the waiting KernelThread is terminated.
  if (object->type() == ObjectType::Thread) {
    return wait_on_object(*object, timeout, waiting_thread_id) ? WaitResult::Success
                                                               : WaitResult::Timeout;
  }
  KernelObject* const raw = object.get();
  std::uint32_t index = 0;
  return wait_for_objects(std::span(&raw, 1), false, timeout, &index, waiting_thread_id,
                          alertable);
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
  std::array<KernelObject*, kMaxWaitObjects> raw{};
  for (std::size_t i = 0; i < objects.size(); ++i) raw[i] = objects[i].get();
  return wait_for_objects(std::span(raw.data(), objects.size()), wait_all, timeout,
                          signaled_index, waiting_thread_id, alertable);
}

WaitResult wait_for_objects(std::span<KernelObject* const> objects, bool wait_all,
                            std::chrono::milliseconds timeout, std::uint32_t* signaled_index,
                            std::uint32_t waiting_thread_id, bool alertable) {
  return wait_for_objects_after(nullptr, objects, wait_all, timeout, signaled_index,
                                waiting_thread_id, alertable);
}

WaitResult signal_and_wait_for_object(const std::function<bool()>& signal,
                                      const std::shared_ptr<KernelObject>& object,
                                      std::chrono::milliseconds timeout,
                                      std::uint32_t waiting_thread_id, bool alertable) {
  if (!object) return WaitResult::Failed;
  KernelObject* const raw = object.get();
  return wait_for_objects_after(&signal, std::span(&raw, 1), false, timeout, nullptr,
                                waiting_thread_id, alertable);
}

namespace {

WaitResult wait_for_objects_after(const std::function<bool()>* signal,
                                  std::span<KernelObject* const> objects, bool wait_all,
                                  std::chrono::milliseconds timeout,
                                  std::uint32_t* signaled_index,
                                  std::uint32_t waiting_thread_id, bool alertable) {
  if (objects.empty() || objects.size() > kMaxWaitObjects) {
    return WaitResult::Failed;
  }
  for (auto* object : objects) {
    if (!object || !is_waitable_object(object->type())) {
      return WaitResult::Failed;
    }
  }

  // Each distinct object once, so a WaitAll consumes a duplicate once and a
  // wait registers once per object.
  std::array<KernelObject*, kMaxWaitObjects> unique_objects{};
  std::copy(objects.begin(), objects.end(), unique_objects.begin());
  const auto unique_end = unique_objects.begin() + static_cast<std::ptrdiff_t>(objects.size());
  std::sort(unique_objects.begin(), unique_end, std::less<KernelObject*>{});
  const std::span<KernelObject* const> distinct(
      unique_objects.begin(), std::unique(unique_objects.begin(), unique_end));

  const auto host_thread = std::this_thread::get_id();
  const auto succeeded = [&](std::uint32_t index, bool abandoned) {
    if (signaled_index && (!wait_all || abandoned)) *signaled_index = index;
    return abandoned ? WaitResult::Abandoned : WaitResult::Success;
  };

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

  // One dispatcher critical section: the optional signal, the immediate
  // check, and registration. A wait that is satisfied at once, ends for an
  // alert or APC, or has a zero timeout, never registers. A satisfiable
  // object has no waiter it could satisfy (a signal satisfies registered
  // waits first), so taking it here takes nobody's signal. Once registered,
  // the signal that makes the wait satisfiable satisfies it
  // (detail::MultiWaitList::satisfy_waiters_locked()). A KernelThread also
  // registers on its own thread, so terminate(), a queued user APC and an
  // alert wake it.
  detail::MultiWaitBlock block;
  block.objects = objects;
  block.distinct = distinct;
  block.wait_all = wait_all;
  block.thread_id = waiting_thread_id;
  block.host_thread = host_thread;
  KernelObject* own_thread = nullptr;
  {
    std::scoped_lock dispatch(detail::dispatcher_mutex());
    if (signal && !(*signal)()) return WaitResult::Failed;
    std::uint32_t index = 0;
    bool abandoned = false;
    if (detail::try_satisfy_wait(objects, distinct, wait_all, waiting_thread_id, host_thread,
                                 index, abandoned)) {
      return succeeded(index, abandoned);
    }
    if (const auto ended = alert_or_apc()) return *ended;
    if (!infinite && timeout.count() == 0) return WaitResult::Timeout;
    for (auto* object : distinct) detail::WaitAccess::wait_list(*object).add(&block);
    if (self && std::find(distinct.begin(), distinct.end(), self) == distinct.end()) {
      own_thread = self;
      detail::WaitAccess::wait_list(*own_thread).add(&block);
    }
  }

  std::optional<WaitResult> outcome;
  while (true) {
    std::uint64_t observed = 0;
    {
      std::scoped_lock lock(block.mutex);
      if (block.granted) break;
      observed = block.wakeups;
    }
    if (terminating()) {
      outcome = WaitResult::Terminating;
      break;
    }
    {
      // A thread object's termination is read, not signalled; take it here.
      std::scoped_lock dispatch(detail::dispatcher_mutex());
      std::uint32_t index = 0;
      bool abandoned = false;
      if (!block.granted &&
          detail::try_satisfy_wait(objects, distinct, wait_all, waiting_thread_id, host_thread,
                                   index, abandoned)) {
        detail::grant(block, index, abandoned);
        break;
      }
    }
    if (const auto ended = alert_or_apc()) {
      outcome = *ended;
      break;
    }
    if (!infinite && std::chrono::steady_clock::now() >= deadline) {
      outcome = WaitResult::Timeout;
      break;
    }

    std::unique_lock lock(block.mutex);
    const auto woken = [&] { return block.wakeups != observed; };
    if (infinite) {
      block.condition.wait(lock, woken);
    } else {
      // On timeout, loop once more: the final check above decides.
      (void)block.condition.wait_until(lock, deadline, woken);
    }
  }

  // Unregistered under the dispatcher lock: a signal that satisfied this wait
  // before then consumed its object for this waiter, so the wait reports it
  // even if it was ending for another reason.
  std::scoped_lock dispatch(detail::dispatcher_mutex());
  for (auto* object : distinct) detail::WaitAccess::wait_list(*object).remove(&block);
  if (own_thread) detail::WaitAccess::wait_list(*own_thread).remove(&block);
  if (block.granted) return succeeded(block.index, block.abandoned);
  return *outcome;
}

}  // namespace

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
