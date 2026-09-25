#include "xenon/kernel/thread.hpp"

#include <algorithm>
#include <cassert>

namespace xenon::kernel {
namespace {

std::atomic<std::uint32_t> g_next_thread_id{1};

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
  // TODO: Map to host thread priority if needed
}

void KernelThread::set_processor_affinity(std::uint32_t affinity) noexcept {
  std::scoped_lock lock(mutex_);
  processor_affinity_ = affinity;
  // TODO: Map to host thread affinity if needed
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
