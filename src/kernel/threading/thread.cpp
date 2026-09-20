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
      processor_affinity_(params.processor_affinity) {
  tls_storage_.fill(0);
}

KernelThread::~KernelThread() {
  if (host_thread_ && host_thread_->joinable()) {
    terminate(0xDEADBEEF);
    host_thread_->join();
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
    state_ = ThreadState::Ready;
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
  return true;
}

bool KernelThread::join(std::uint32_t timeout_ms) {
  if (host_thread_ && host_thread_->joinable()) {
    if (timeout_ms == 0xFFFFFFFF) {
      host_thread_->join();
      return true;
    } else {
      // TODO: Implement timeout support
      host_thread_->join();
      return true;
    }
  }
  return false;
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
  {
    std::scoped_lock lock(mutex_);
    state_ = ThreadState::Running;
  }

  std::uint32_t result = 0;
  if (entry_) {
    result = entry_();
  }

  {
    std::scoped_lock lock(mutex_);
    exit_code_ = result;
    state_ = ThreadState::Terminated;
  }
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

  // Terminate and join all threads outside the lock
  for (auto& thread : threads_copy) {
    thread->terminate(0);
    thread->join();
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
