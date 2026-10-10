#include "xenon/kernel/thread.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>

#include "kernel/synchronization/wait_internal.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace xenon::kernel {
namespace {

std::atomic<std::uint32_t> g_next_thread_id{1};

#if defined(_WIN32)
// Real Xbox 360 hardware is 3 physical cores x 2 hardware threads = 6
// logical processors, and guest code (job-pool workers in particular - see
// e.g. Ace Combat 6's JobPoolA/JobPoolB) routinely relies on the resulting
// deterministic scheduling for correctness, not just throughput: a submitter
// that writes a job's parameters and only then publishes it to a worker is
// safe on real hardware because the fixed core assignment and guest thread
// priorities mean the two never truly race. A host that runs every guest
// thread as an ordinary, unprioritized, unpinned OS thread turns that into a
// real, host-timing-dependent race. Honoring the guest's own priority/
// affinity requests - which Xenon already tracks but previously discarded -
// restores enough of that scheduling determinism to close such races instead
// of only reproducing them.
int windows_thread_priority(ThreadPriority priority) noexcept {
  switch (priority) {
    case ThreadPriority::Idle: return THREAD_PRIORITY_IDLE;
    case ThreadPriority::Lowest: return THREAD_PRIORITY_LOWEST;
    case ThreadPriority::BelowNormal: return THREAD_PRIORITY_BELOW_NORMAL;
    case ThreadPriority::Normal: return THREAD_PRIORITY_NORMAL;
    case ThreadPriority::AboveNormal: return THREAD_PRIORITY_ABOVE_NORMAL;
    case ThreadPriority::Highest: return THREAD_PRIORITY_HIGHEST;
    case ThreadPriority::TimeCritical: return THREAD_PRIORITY_TIME_CRITICAL;
  }
  return THREAD_PRIORITY_NORMAL;
}

void apply_host_priority(std::thread& host_thread, ThreadPriority priority) noexcept {
  const auto handle = static_cast<HANDLE>(host_thread.native_handle());
  if (!handle) return;
  SetThreadPriority(handle, windows_thread_priority(priority));
}

void apply_host_affinity(std::thread& host_thread, std::uint32_t affinity) noexcept {
  // 0xFFFFFFFF is ThreadCreationParams's "no restriction requested" sentinel
  // (see thread.hpp) - leave the host's own default affinity untouched
  // rather than pinning every unrestricted thread to the low bits of
  // whatever the host happens to have.
  if (affinity == 0xFFFFFFFFu) return;
  const auto handle = static_cast<HANDLE>(host_thread.native_handle());
  if (!handle) return;
  // Xbox 360 hardware-thread bit N maps directly to host logical-processor
  // bit N; every realistic host has at least the 6 bits real hardware could
  // ever set, so no renumbering is needed - just mask to what the host
  // actually reports, in case a guest affinity bit falls outside a very
  // small host's processor count.
  SYSTEM_INFO system_info{};
  GetSystemInfo(&system_info);
  const DWORD_PTR host_mask =
      system_info.dwNumberOfProcessors >= 32
          ? 0xFFFFFFFFu
          : ((DWORD_PTR{1} << system_info.dwNumberOfProcessors) - 1);
  const DWORD_PTR requested = static_cast<DWORD_PTR>(affinity) & host_mask;
  if (requested == 0) return;
  SetThreadAffinityMask(handle, requested);
}
#endif

}  // namespace

KernelThread::KernelThread(ThreadEntry entry, const ThreadCreationParams& params)
    : KernelObject(ObjectType::Thread),
      entry_(std::move(entry)),
      name_(params.name),
      thread_id_(g_next_thread_id.fetch_add(1, std::memory_order_relaxed)),
      stack_size_(params.stack_size),
      creation_flags_(params.creation_flags),
      priority_(params.priority),
      processor_affinity_(params.processor_affinity),
      create_suspended_(params.create_suspended),
      completion_future_(completion_promise_.get_future().share()) {
  tls_storage_.fill(0);
  if (create_suspended_) {
    suspend_count_.store(1, std::memory_order_relaxed);
  }
}

KernelThread::~KernelThread() {
  if (host_thread_) {
    terminate(0xDEADBEEF);
    // A bounded wait, not the previous unconditional host_thread_->join():
    // terminate() cannot interrupt a thread already running non-preemptible
    // compiled guest code (see terminate()'s own doc comment), so an
    // unconditional join here could hang process teardown forever on a
    // wedged guest thread. Detach on timeout instead - the host thread
    // continues in the background until the process exits; there is no
    // standard-C++ way to force-kill another thread.
    constexpr std::uint32_t kDestructorJoinTimeoutMs = 5000;
    if (!join(kDestructorJoinTimeoutMs) && host_thread_->joinable()) {
      host_thread_->detach();
    }
  }
}

ThreadState KernelThread::state() const noexcept {
  std::scoped_lock lock(mutex_);
  return state_;
}

ThreadPriority KernelThread::priority() const noexcept {
  std::scoped_lock lock(mutex_);
  return priority_;
}

std::uint32_t KernelThread::exit_code() const noexcept {
  std::scoped_lock lock(mutex_);
  return exit_code_;
}

std::uint32_t KernelThread::processor_affinity() const noexcept {
  std::scoped_lock lock(mutex_);
  return processor_affinity_;
}

void KernelThread::set_priority(ThreadPriority priority) noexcept {
  std::scoped_lock lock(mutex_);
  priority_ = priority;
#if defined(_WIN32)
  if (host_thread_) apply_host_priority(*host_thread_, priority_);
#endif
}

void KernelThread::set_processor_affinity(std::uint32_t affinity) noexcept {
  std::scoped_lock lock(mutex_);
  processor_affinity_ = affinity;
#if defined(_WIN32)
  if (host_thread_) apply_host_affinity(*host_thread_, processor_affinity_);
#endif
}

void KernelThread::set_name(std::string name) {
  std::scoped_lock lock(mutex_);
  name_ = std::move(name);
}

bool KernelThread::start() {
  std::scoped_lock lock(mutex_);
  if (state_ != ThreadState::Initialized) {
    return false;
  }

  try {
    host_thread_ = std::make_unique<std::thread>(&KernelThread::thread_main, this);
#if defined(_WIN32)
    apply_host_priority(*host_thread_, priority_);
    apply_host_affinity(*host_thread_, processor_affinity_);
#endif
    // create_suspended_ threads report Suspended (not Ready) as soon as the
    // host thread is spawned - matching real CREATE_SUSPENDED semantics,
    // where the thread exists and is queryable/resumable immediately, even
    // though thread_main() itself is parked and has not yet run entry_().
    state_ = create_suspended_ ? ThreadState::Suspended : ThreadState::Ready;
    return true;
  } catch (...) {
    return false;
  }
}

bool KernelThread::suspend() {
  std::scoped_lock lock(mutex_);
  if (state_ == ThreadState::Terminated) {
    return false;
  }

  const auto old_count = suspend_count_.fetch_add(1, std::memory_order_release);
  if (old_count == 0) {
    state_ = ThreadState::Suspended;
  }
  return true;
}

bool KernelThread::resume() {
  std::scoped_lock lock(mutex_);
  if (state_ == ThreadState::Terminated) {
    return false;
  }

  const auto old_count = suspend_count_.load(std::memory_order_acquire);
  {
    if (FILE* _d = std::fopen("thread_resume_diag.log", "a")) {
      std::fprintf(_d, "resume() called: thread_id=%u old_suspend_count=%d\n",
                   thread_id_, (int)old_count);
      std::fclose(_d);
    }
  }
  if (old_count == 0) {
    return false;  // Not suspended
  }

  const auto new_count = suspend_count_.fetch_sub(1, std::memory_order_release);
  if (new_count == 1) {  // Was 1, now 0
    state_ = ThreadState::Ready;
    // Wakes a thread_main() still parked at entry (create_suspended, never
    // yet resumed) so it can proceed to Running and call entry_(). A no-op
    // wait notification for a thread that is already past that point.
    suspend_condition_.notify_all();
  }
  return true;
}

bool KernelThread::terminate(std::uint32_t exit_code) {
  std::scoped_lock lock(mutex_);
  if (state_ == ThreadState::Terminated) {
    return false;
  }

  exit_code_ = exit_code;
  state_ = ThreadState::Terminated;
  terminated_.store(true, std::memory_order_release);
  detail::notify_multi_object_waiters_unconditionally();
  // Wakes a thread_main() parked at entry (create_suspended, never resumed),
  // or mid-execution at a dispatch_guest_thread() safepoint via
  // wait_while_suspended(), so it observes Terminated and exits instead of
  // parking forever - real CREATE_SUSPENDED threads can be terminated
  // without ever having run, and a suspended running thread can be
  // terminated without ever being resumed.
  suspend_condition_.notify_all();
  return true;
}

void KernelThread::wait_while_suspended() {
  // Fast path: no lock needed when not suspended, so calling this on every
  // dispatch-loop iteration (see docs/kernel/THREADING_V2.md) does not add
  // mutex contention to the common case.
  if (suspend_count_.load(std::memory_order_acquire) == 0) {
    return;
  }
  std::unique_lock lock(mutex_);
  suspend_condition_.wait(lock, [this] {
    return suspend_count_.load(std::memory_order_acquire) == 0 ||
           state_ == ThreadState::Terminated;
  });
}

bool KernelThread::join(std::uint32_t timeout_ms) {
  if (!host_thread_) {
    return false;
  }

  // A thread must never join itself: std::thread::join() is documented to
  // throw std::system_error(resource_deadlock_would_occur) when called with
  // get_id()==std::this_thread::get_id(), and this is genuinely reachable
  // here - the last std::shared_ptr<KernelThread> reference can drop (e.g. a
  // temporary from ThreadManager::get_thread() going out of scope) while
  // still executing guest code ON this same host thread, late in
  // thread_main()'s own natural completion, running this destructor/join
  // synchronously on itself. Detach instead: the host thread is already at
  // (or extremely close to) completion_future_ being ready in that case, so
  // there is nothing left to actually wait for.
  if (std::this_thread::get_id() == host_thread_->get_id()) {
    if (host_thread_->joinable()) {
      host_thread_->detach();
    }
    return true;
  }

  // completion_future_ is set exactly once by thread_main() right before it
  // returns, on every exit path (entry_() returned naturally, or the thread
  // was terminated before ever running entry_()) - so waiting on it, rather
  // than on host_thread_ directly, gives a real, honored timeout instead of
  // always blocking unconditionally.
  if (timeout_ms == 0xFFFFFFFFu) {
    completion_future_.wait();
  } else if (completion_future_.wait_for(std::chrono::milliseconds(timeout_ms)) ==
             std::future_status::timeout) {
    return false;
  }

  // The host thread has already finished running (or was terminated before
  // starting) by the time completion_future_ is ready, so this is a fast,
  // bounded cleanup join, not an unbounded wait. Guarded by mutex_ because
  // join() may legitimately be called concurrently by more than one waiter
  // (e.g. two guest threads both calling NtWaitForSingleObjectEx on the same
  // thread handle) - std::thread::join() itself is not safe to call
  // concurrently from multiple callers on the same std::thread object.
  std::scoped_lock lock(mutex_);
  if (host_thread_ && host_thread_->joinable()) {
    host_thread_->join();
  }
  return true;
}

bool KernelThread::is_current_host_thread() const noexcept {
  return host_thread_ && std::this_thread::get_id() == host_thread_->get_id();
}

std::optional<std::uint64_t> KernelThread::get_tls(std::uint32_t slot) const {
  if (slot >= kTlsSlotCount) {
    return std::nullopt;
  }
  std::scoped_lock lock(mutex_);
  return tls_storage_[slot];
}

bool KernelThread::set_tls(std::uint32_t slot, std::uint64_t value) {
  if (slot >= kTlsSlotCount) {
    return false;
  }
  std::scoped_lock lock(mutex_);
  tls_storage_[slot] = value;
  return true;
}

void KernelThread::thread_main() {
  // Honors CREATE_SUSPENDED: park here until resume() drops suspend_count_
  // to zero, or terminate() is called while still parked - either way, this
  // never runs entry_() before the thread has actually been resumed at
  // least once, matching real Xbox 360/Win32 semantics. Shares its
  // park/wake mechanism with the mid-execution safepoint
  // dispatch_guest_thread() calls via wait_while_suspended() directly.
  wait_while_suspended();

  bool should_run_entry = false;
  {
    std::scoped_lock lock(mutex_);
    if (state_ != ThreadState::Terminated) {
      state_ = ThreadState::Running;
      should_run_entry = true;
    }
  }

  {
    if (FILE* _d = std::fopen("thread_main_entry_diag.log", "a")) {
      std::fprintf(_d, "thread_main: thread_id=%u should_run_entry=%d has_entry=%d\n",
                   thread_id_, should_run_entry ? 1 : 0, entry_ ? 1 : 0);
      std::fclose(_d);
    }
  }
  std::uint32_t result = 0;
  if (should_run_entry && entry_) {
    result = entry_();
  }

  {
    std::scoped_lock lock(mutex_);
    // A concurrent terminate() may already have set a specific exit_code_
    // and Terminated state while entry_() was still running (or while this
    // thread was still parked above) - do not silently clobber that with
    // entry_()'s natural return value. This is the fix for the historical
    // race where a caller that believed it had authoritatively terminated
    // the thread would see its exit code overwritten the moment entry_()
    // happened to return naturally afterward.
    if (state_ != ThreadState::Terminated) {
      exit_code_ = result;
      state_ = ThreadState::Terminated;
      terminated_.store(true, std::memory_order_release);
      detail::notify_multi_object_waiters_unconditionally();
    }
  }
  completion_promise_.set_value();
}

// ThreadManager implementation

thread_local std::shared_ptr<KernelThread> ThreadManager::current_thread_;

ThreadManager::~ThreadManager() {
  shutdown();
}

std::shared_ptr<KernelThread> ThreadManager::create_thread(
    ThreadEntry entry, const ThreadCreationParams& params) {
  // Opportunistic housekeeping: reap any threads that finished since the
  // last time something triggered cleanup. Must run BEFORE acquiring
  // mutex_ below - reap_finished_threads() takes it itself, and it is not
  // recursive.
  reap_finished_threads();

  std::scoped_lock lock(mutex_);

  auto thread = std::make_shared<KernelThread>(std::move(entry), params);
  threads_[thread->thread_id()] = thread;
  
  return thread;
}

std::shared_ptr<KernelThread> ThreadManager::get_thread(std::uint32_t thread_id) const {
  std::scoped_lock lock(mutex_);
  auto it = threads_.find(thread_id);
  return it != threads_.end() ? it->second : nullptr;
}

KernelThread* ThreadManager::get_thread_ptr(std::uint32_t thread_id) const noexcept {
  std::scoped_lock lock(mutex_);
  auto it = threads_.find(thread_id);
  return it != threads_.end() ? it->second.get() : nullptr;
}

std::shared_ptr<KernelThread> ThreadManager::current_thread() const {
  return current_thread_;
}

void ThreadManager::set_current_thread(std::shared_ptr<KernelThread> thread) {
  current_thread_ = std::move(thread);
}

void ThreadManager::remove_thread(std::uint32_t thread_id) {
  std::scoped_lock lock(mutex_);
  threads_.erase(thread_id);
}

std::size_t ThreadManager::reap_finished_threads() {
  std::vector<std::shared_ptr<KernelThread>> finished;
  {
    std::scoped_lock lock(mutex_);
    for (auto it = threads_.begin(); it != threads_.end();) {
      // is_current_host_thread() (not merely !is_terminated()) is what
      // makes this safe to call from within a still-running thread's own
      // entry_(): terminate() can set is_terminated() from another thread
      // while this thread is still executing (a mid-dispatch preemptive
      // terminate), so is_terminated() alone cannot tell "I just finished"
      // apart from "I am still on my own stack, marked terminated out from
      // under me". A thread can never be its own is_current_host_thread()
      // match while calling this, so it can never reap itself.
      if (it->second->is_terminated() && !it->second->is_current_host_thread()) {
        finished.push_back(std::move(it->second));
        it = threads_.erase(it);
      } else {
        ++it;
      }
    }
  }
  // `finished` is destroyed here, outside mutex_. If ThreadManager held the
  // last reference to a given thread, this synchronously runs
  // ~KernelThread(), which already performs its own bounded
  // terminate()+join(5s)+detach-on-timeout (see the destructor) - the same
  // shape of cleanup shutdown() relies on. Deliberately not join()-ing
  // again first: is_terminated()==true does not guarantee the host thread
  // has actually returned (terminate() cannot interrupt non-preemptible
  // compiled guest code already in flight), and an extra untimed join here
  // could stall whatever called reap_finished_threads() (e.g.
  // create_thread(), on what may be a hot path) on a wedged thread.
  return finished.size();
}

void ThreadManager::shutdown() {
  std::vector<std::shared_ptr<KernelThread>> threads_copy;
  {
    std::scoped_lock lock(mutex_);
    threads_copy.reserve(threads_.size());
    for (auto& [id, thread] : threads_) {
      threads_copy.push_back(thread);
    }
    threads_.clear();
  }

  // Terminate and join all threads outside the lock. Bounded, not
  // unconditional: a thread stuck running non-preemptible compiled guest
  // code must not be able to hang process/session teardown forever (see
  // KernelThread's destructor, which applies the same bound and detaches on
  // timeout - reached here too if a thread is still joinable when
  // threads_copy is destroyed at the end of this function).
  constexpr std::uint32_t kShutdownJoinTimeoutMs = 5000;
  for (auto& thread : threads_copy) {
    thread->terminate(0);
    static_cast<void>(thread->join(kShutdownJoinTimeoutMs));
  }
}

std::vector<std::shared_ptr<KernelThread>> ThreadManager::enumerate_threads() const {
  std::scoped_lock lock(mutex_);
  std::vector<std::shared_ptr<KernelThread>> result;
  result.reserve(threads_.size());
  for (const auto& [id, thread] : threads_) {
    result.push_back(thread);
  }
  return result;
}

std::size_t ThreadManager::thread_count() const {
  std::scoped_lock lock(mutex_);
  return threads_.size();
}

}  // namespace xenon::kernel
