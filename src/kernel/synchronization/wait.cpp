#include "xenon/kernel/wait.hpp"

#include "xenon/kernel/event.hpp"
#include "xenon/kernel/mutant.hpp"
#include "xenon/kernel/semaphore.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/kernel/timer.hpp"
#include "xenon/kernel/wait_profile.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sched.h>
#endif

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#elif defined(_M_ARM64)
#include <intrin.h>
#endif

namespace xenon::kernel {

namespace detail {

std::mutex& dispatcher_mutex() noexcept {
  // Never destroyed: guest threads may still signal during process teardown.
  static auto* mutex = new std::mutex();
  return *mutex;
}

#if defined(_WIN32)
// An auto-reset event the wakers set, and a high-resolution waitable timer
// for the deadline (Windows 10 1803+; an ordinary waitable timer before
// that). Both take 100 ns due times; the high-resolution one also fires at
// that precision rather than on the next scheduler tick, which a
// condition_variable wait (rounded up to whole milliseconds by the STL, then
// to the timer tick) cannot do.
class HostParker {
 public:
  HostParker() noexcept
      : event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
        timer_(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS)) {
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }
  ~HostParker() {
    if (timer_) CloseHandle(timer_);
    if (event_) CloseHandle(event_);
  }
  HostParker(const HostParker&) = delete;
  HostParker& operator=(const HostParker&) = delete;

  void unpark() noexcept {
    if (event_) SetEvent(event_);
  }

  void park() noexcept {
    if (!event_) {
      // No event could be created: a park may return spuriously, so a yield
      // is a correct (if costly) park.
      SwitchToThread();
      return;
    }
    WaitForSingleObject(event_, INFINITE);
  }

  void park_until(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) return;
    if (!event_) {
      SwitchToThread();
      return;
    }
    using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
    LARGE_INTEGER due{};
    // Negative: relative to now, in 100 ns units, rounded up so the timer
    // never fires before the deadline.
    due.QuadPart = -(std::max)(std::chrono::ceil<Ticks>(remaining).count(), std::int64_t{1});
    if (timer_ && SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) {
      const HANDLE handles[2] = {event_, timer_};
      WaitForMultipleObjects(2, handles, FALSE, INFINITE);
      return;
    }
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    WaitForSingleObject(event_, static_cast<DWORD>((std::min)(ms, std::int64_t{0xFFFFFFFE})));
  }

 private:
  HANDLE event_;
  HANDLE timer_;
};
#else
class HostParker {
 public:
  void unpark() noexcept {
    {
      std::scoped_lock lock(mutex_);
      notified_ = true;
    }
    condition_.notify_one();
  }
  void park() noexcept {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return notified_; });
    notified_ = false;
  }
  void park_until(std::chrono::steady_clock::time_point deadline) noexcept {
    std::unique_lock lock(mutex_);
    (void)condition_.wait_until(lock, deadline, [this] { return notified_; });
    notified_ = false;
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool notified_{false};
};
#endif

namespace {

HostParker& this_thread_parker() noexcept {
  thread_local HostParker parker;
  return parker;
}

void cpu_relax() noexcept {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#elif defined(_M_ARM64)
  __yield();
#elif defined(__aarch64__)
  asm volatile("yield");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// Records a change the waiter must re-check, and wakes it if it is parked.
// The seq_cst increment and the seq_cst read of `parked` pair with the
// waiter's seq_cst store of `parked` and re-read of `wakeups`.
void wake(MultiWaitBlock& block) noexcept {
  block.wakeups.fetch_add(1, std::memory_order_seq_cst);
  if (block.parked.load(std::memory_order_seq_cst) && block.parker) block.parker->unpark();
}

}  // namespace

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
  block.index = index;
  block.abandoned = abandoned;
  block.granted.store(true, std::memory_order_release);
  wake(block);
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
  // A block this could satisfy is only added or removed with the dispatcher
  // lock held, as here, so a zero size read under it means there is none;
  // a block removed without it (granted, or a delay's) needs nothing here.
  if (size_.load(std::memory_order_relaxed) == 0) return;
  std::scoped_lock lock(mutex_);
  for (auto* block : blocks_) {
    if (block->granted.load(std::memory_order_relaxed) || block->objects.empty()) continue;
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
  for (auto* block : blocks_) wake(*block);
}

namespace {
constexpr std::int32_t kSpinScoreMax = 64;
constexpr std::int32_t kSpinHitCredit = 8;
constexpr std::int32_t kSpinMissDebit = 1;
// With no credit left, one wait in this many still spins, as a probe.
constexpr std::uint32_t kSpinProbeInterval = 16;
}  // namespace

bool MultiWaitList::spin_advised() noexcept {
  if (spin_score_.load(std::memory_order_relaxed) > 0) return true;
  return spin_probe_.fetch_add(1, std::memory_order_relaxed) % kSpinProbeInterval == 0;
}

void MultiWaitList::note_spin(bool satisfied) noexcept {
  // A heuristic: concurrent updates may lose one another, never corrupt it.
  const auto score = spin_score_.load(std::memory_order_relaxed);
  const auto next = satisfied ? (std::min)(score + kSpinHitCredit, kSpinScoreMax)
                              : (std::max)(score - kSpinMissDebit, std::int32_t{0});
  if (next != score) spin_score_.store(next, std::memory_order_relaxed);
}

}  // namespace detail

namespace {

constexpr std::size_t kMaxWaitObjects = 64;
constexpr std::int64_t kDefaultSpinNs = 4'000;
constexpr std::int64_t kMaxSpinNs = 100'000;
// A delay with less than this left yields until its deadline instead of
// parking: even a high-resolution host timer fires on the host's timer step
// (0.5 ms on current Windows), so blocking for less would overshoot it
// several times over.
constexpr auto kShortestBlock = std::chrono::microseconds(200);

std::int64_t initial_spin_limit_ns() noexcept {
  const char* setting = std::getenv("XENON_WAIT_SPIN");
  if (!setting || !*setting) return kDefaultSpinNs;
  char* end = nullptr;
  const long us = std::strtol(setting, &end, 10);
  if (end == setting || us < 0) return kDefaultSpinNs;
  return (std::min)(static_cast<std::int64_t>(us) * 1'000, kMaxSpinNs);
}

std::atomic<std::int64_t>& spin_limit_ns() noexcept {
  static std::atomic<std::int64_t> limit{initial_spin_limit_ns()};
  return limit;
}

bool multiprocessor_host() noexcept {
  static const bool multi = std::thread::hardware_concurrency() != 1;
  return multi;
}

// Infinite: a negative timeout, or 0xFFFFFFFF ms (INFINITE) or more.
bool is_infinite(std::chrono::nanoseconds timeout) noexcept {
  return timeout.count() < 0 || timeout >= std::chrono::milliseconds(0xFFFFFFFFll);
}

// The wait under every wait. `signal`, when given, runs first, in the same
// dispatcher critical section that checks and registers the wait.
WaitResult wait_for_objects_after(const std::function<bool()>* signal,
                                  std::span<KernelObject* const> objects, bool wait_all,
                                  std::chrono::nanoseconds timeout,
                                  std::uint32_t* signaled_index,
                                  std::uint32_t waiting_thread_id, bool alertable);

}  // namespace

std::chrono::nanoseconds wait_spin_limit() noexcept {
  return std::chrono::nanoseconds(spin_limit_ns().load(std::memory_order_relaxed));
}

void set_wait_spin_limit(std::chrono::nanoseconds limit) noexcept {
  const auto ns = std::clamp<std::int64_t>(limit.count(), 0, kMaxSpinNs);
  spin_limit_ns().store(ns, std::memory_order_relaxed);
}

bool yield_current_thread() noexcept {
#if defined(_WIN32)
  return SwitchToThread() != FALSE;
#else
  return sched_yield() == 0;
#endif
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

std::size_t waiter_count(KernelObject& object) {
  if (!is_waitable_object(object.type())) return 0;
  return detail::WaitAccess::wait_list(object).size();
}

bool wait_on_object(KernelObject& object, std::chrono::nanoseconds timeout,
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
      // Waiting on a thread means waiting for it to terminate. join() counts
      // whole milliseconds: a finite timeout rounds up, so it is never early.
      auto& thread = static_cast<KernelThread&>(object);
      std::uint32_t timeout_ms = 0xFFFFFFFFu;
      if (!is_infinite(timeout)) {
        timeout_ms = static_cast<std::uint32_t>((std::min)(
            std::chrono::ceil<std::chrono::milliseconds>(timeout).count(), std::int64_t{0xFFFFFFFE}));
      }
      return thread.state() == ThreadState::Terminated || thread.join(timeout_ms);
    }
    default:
      return false;
  }
}

WaitResult wait_for_single_object(
    const std::shared_ptr<KernelObject>& object,
    std::chrono::nanoseconds timeout,
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
    std::chrono::nanoseconds timeout,
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
                            std::chrono::nanoseconds timeout, std::uint32_t* signaled_index,
                            std::uint32_t waiting_thread_id, bool alertable) {
  return wait_for_objects_after(nullptr, objects, wait_all, timeout, signaled_index,
                                waiting_thread_id, alertable);
}

WaitResult signal_and_wait_for_object(const std::function<bool()>& signal,
                                      const std::shared_ptr<KernelObject>& object,
                                      std::chrono::nanoseconds timeout,
                                      std::uint32_t waiting_thread_id, bool alertable) {
  if (!object) return WaitResult::Failed;
  KernelObject* const raw = object.get();
  return wait_for_objects_after(&signal, std::span(&raw, 1), false, timeout, nullptr,
                                waiting_thread_id, alertable);
}

namespace {

// How long a registered wait on `distinct` spins before it blocks: zero when
// spinning is off, the host has one processor, or none of the objects'
// recent spins advise it; never past the deadline.
std::chrono::nanoseconds spin_budget(std::span<KernelObject* const> distinct, bool infinite,
                                     std::chrono::steady_clock::time_point deadline) {
  const auto limit = spin_limit_ns().load(std::memory_order_relaxed);
  if (limit <= 0 || !multiprocessor_host()) return std::chrono::nanoseconds::zero();
  bool advised = false;
  for (auto* object : distinct) {
    if (detail::WaitAccess::wait_list(*object).spin_advised()) advised = true;
  }
  if (!advised) return std::chrono::nanoseconds::zero();
  auto budget = std::chrono::nanoseconds(limit);
  if (!infinite) {
    budget = (std::min)(budget, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    deadline - std::chrono::steady_clock::now()));
  }
  return (std::max)(budget, std::chrono::nanoseconds::zero());
}

WaitResult wait_for_objects_after(const std::function<bool()>* signal,
                                  std::span<KernelObject* const> objects, bool wait_all,
                                  std::chrono::nanoseconds timeout,
                                  std::uint32_t* signaled_index,
                                  std::uint32_t waiting_thread_id, bool alertable) {
  if (objects.empty() || objects.size() > kMaxWaitObjects) {
    return WaitResult::Failed;
  }
  bool watches_thread = false;
  for (auto* object : objects) {
    if (!object || !is_waitable_object(object->type())) {
      return WaitResult::Failed;
    }
    if (object->type() == ObjectType::Thread) watches_thread = true;
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

  const bool infinite = is_infinite(timeout);
  const auto deadline =
      std::chrono::steady_clock::now() + (infinite ? std::chrono::nanoseconds(0) : timeout);

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
  auto& parker = detail::this_thread_parker();
  detail::MultiWaitBlock block;
  block.objects = objects;
  block.distinct = distinct;
  block.wait_all = wait_all;
  block.thread_id = waiting_thread_id;
  block.host_thread = host_thread;
  block.parker = &parker;
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

  // Registered: the wait blocks from here (wait attribution, wait_profile.hpp).
  const auto registered_at = std::chrono::steady_clock::now();
  const bool profiling = wait_profile_enabled();

  // Spin first, watching this wait's own block: a signaller that satisfies
  // the wait meanwhile grants it here, under the dispatcher lock, exactly as
  // it would a parked waiter; the spin only reads atomics, takes no lock,
  // and costs the signaller no wakeup call. It stops early for any wakeup
  // (a grant, terminate(), an APC, an alert), which the loop below handles.
  const auto budget = spin_budget(distinct, infinite, deadline);
  bool spun = false;
  bool spin_hit = false;
  std::uint64_t spin_ns = 0;
  if (budget > std::chrono::nanoseconds::zero()) {
    spun = true;
    const auto observed = block.wakeups.load(std::memory_order_acquire);
    const auto spin_end = registered_at + budget;
    auto now = registered_at;
    while (block.wakeups.load(std::memory_order_acquire) == observed) {
      for (int i = 0; i < 8; ++i) detail::cpu_relax();
      now = std::chrono::steady_clock::now();
      if (now >= spin_end) break;
    }
    spin_hit = block.granted.load(std::memory_order_acquire);
    spin_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - registered_at).count());
    if (spin_hit) {
      // Credit what ended the wait: a WaitAny's granted object, a WaitAll's
      // every object.
      if (wait_all) {
        for (auto* object : distinct) detail::WaitAccess::wait_list(*object).note_spin(true);
      } else if (block.index < objects.size()) {
        detail::WaitAccess::wait_list(*objects[block.index]).note_spin(true);
      }
    }
  }

  std::optional<WaitResult> outcome;
  bool blocked = false;
  while (true) {
    const auto observed = block.wakeups.load(std::memory_order_seq_cst);
    if (block.granted.load(std::memory_order_acquire)) break;
    if (terminating()) {
      outcome = WaitResult::Terminating;
      break;
    }
    if (watches_thread) {
      // A thread object's termination is read, not signalled: take it here.
      // Every other object's change is a signal that satisfies the
      // registered wait itself, so no other wait needs the dispatcher lock.
      std::scoped_lock dispatch(detail::dispatcher_mutex());
      std::uint32_t index = 0;
      bool abandoned = false;
      if (!block.granted.load(std::memory_order_relaxed) &&
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

    // Park unless a wakeup arrived since `observed`; see MultiWaitBlock.
    block.parked.store(true, std::memory_order_seq_cst);
    if (block.wakeups.load(std::memory_order_seq_cst) == observed) {
      blocked = true;
      if (infinite) {
        parker.park();
      } else {
        // On timeout, loop once more: the final checks above decide.
        parker.park_until(deadline);
      }
    }
    block.parked.store(false, std::memory_order_relaxed);
  }
  if (spun && !spin_hit) {
    for (auto* object : distinct) detail::WaitAccess::wait_list(*object).note_spin(false);
  }

  // Unregistered. A granted block can no longer be satisfied, and every
  // signaller reaches a block only under the list lock that remove() takes,
  // so it leaves without the dispatcher lock. Any other wait unregisters
  // under the dispatcher lock: a signal that satisfied it before then
  // consumed its object for this waiter, so the wait reports it even if it
  // was ending for another reason.
  WaitResult result{};
  if (block.granted.load(std::memory_order_acquire)) {
    for (auto* object : distinct) detail::WaitAccess::wait_list(*object).remove(&block);
    if (own_thread) detail::WaitAccess::wait_list(*own_thread).remove(&block);
    result = succeeded(block.index, block.abandoned);
  } else {
    std::scoped_lock dispatch(detail::dispatcher_mutex());
    for (auto* object : distinct) detail::WaitAccess::wait_list(*object).remove(&block);
    if (own_thread) detail::WaitAccess::wait_list(*own_thread).remove(&block);
    result = block.granted.load(std::memory_order_relaxed) ? succeeded(block.index, block.abandoned)
                                                           : *outcome;
  }
  note_registered_wait(spun, spin_hit, blocked, spin_ns);
  if (profiling) {
    // The object that ended the wait: the one granted, else the first.
    const bool took = result == WaitResult::Success || result == WaitResult::Abandoned;
    const auto index = took && block.index < objects.size() ? block.index : 0u;
    note_blocking_wait(waiting_thread_id, *objects[index], result,
                       static_cast<std::uint64_t>(
                           std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - registered_at)
                               .count()),
                       spin_hit);
  }
  return result;
}

}  // namespace

WaitResult delay_current_thread(std::chrono::nanoseconds delay, bool alertable) {
  auto* self = KernelThread::current();
  const auto apc_pending = [&] { return alertable && self && self->user_apc_pending(); };
  const auto ended = [&]() -> std::optional<WaitResult> {
    if (!self) return std::nullopt;
    if (self->is_terminated()) return WaitResult::Terminating;
    if (alertable && self->take_alert()) return WaitResult::Alerted;
    if (apc_pending()) return WaitResult::UserApc;
    return std::nullopt;
  };
  if (const auto result = ended()) return *result;
  // A zero (or past) interval gives up the rest of the time slice, as on
  // the Xbox 360, rather than returning at once.
  if (delay.count() <= 0) {
    yield_current_thread();
    return WaitResult::Success;
  }

  // A delay beyond any run saturates rather than overflow the clock.
  constexpr auto kLongestDelay = std::chrono::hours(24 * 365 * 100);
  const auto deadline = std::chrono::steady_clock::now() +
                        (std::min)(delay, std::chrono::nanoseconds(kLongestDelay));
  auto& parker = detail::this_thread_parker();
  // A KernelThread registers on its own list, so terminate(), a queued APC
  // and an alert wake it. The block has no objects: nothing satisfies it,
  // and it is added and removed under its list's lock alone.
  detail::MultiWaitBlock block;
  block.parker = &parker;
  detail::MultiWaitList* own = self ? &detail::WaitAccess::wait_list(*self) : nullptr;
  if (own) own->add(&block);
  struct Unregister {
    detail::MultiWaitList* list;
    detail::MultiWaitBlock* block;
    ~Unregister() {
      if (list) list->remove(block);
    }
  } unregister{own, &block};

  // A delay gives the processor up at least once, however short it is.
  bool gave_up = false;
  while (true) {
    const auto observed = block.wakeups.load(std::memory_order_seq_cst);
    // Registered before these checks, so a terminate(), APC or alert after
    // them wakes the park below.
    if (const auto result = ended()) return *result;
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      if (!gave_up) yield_current_thread();
      return WaitResult::Success;
    }
    gave_up = true;
    if (remaining < kShortestBlock) {
      // Too short to block for precisely: give the processor up until the
      // deadline instead.
      yield_current_thread();
      continue;
    }
    block.parked.store(true, std::memory_order_seq_cst);
    if (block.wakeups.load(std::memory_order_seq_cst) == observed) parker.park_until(deadline);
    block.parked.store(false, std::memory_order_relaxed);
  }
}

}  // namespace xenon::kernel
