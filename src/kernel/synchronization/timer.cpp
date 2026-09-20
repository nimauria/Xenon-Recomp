#include "xenon/kernel/timer.hpp"

namespace xenon::kernel {

KernelTimer::KernelTimer(TimerType type)
    : KernelObject(ObjectType::Timer), type_(type) {}

bool KernelTimer::set(std::chrono::milliseconds due_time,
                     std::optional<std::chrono::milliseconds> period,
                     TimerCallback callback) {
  std::scoped_lock lock(mutex_);
  
  due_time_ = std::chrono::steady_clock::now() + due_time;
  period_ = period;
  callback_ = std::move(callback);
  active_ = true;
  signaled_ = false;

  // TODO: In a full implementation, this would start a timer thread
  // For now, we just mark it as signaled immediately if due_time is 0
  if (due_time.count() == 0) {
    signaled_ = true;
    condition_.notify_all();
    if (callback_) {
      callback_();
    }
  }

  return true;
}

bool KernelTimer::cancel() {
  std::scoped_lock lock(mutex_);
  
  if (!active_) {
    return false;
  }

  active_ = false;
  signaled_ = false;
  condition_.notify_all();
  return true;
}

bool KernelTimer::wait_for(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  
  if (!condition_.wait_for(lock, timeout, [&] { return signaled_; })) {
    return false;
  }

  // Auto-reset for synchronization timers
  if (type_ == TimerType::SynchronizationTimer) {
    signaled_ = false;
  }

  return true;
}

bool KernelTimer::is_signaled() const {
  std::scoped_lock lock(mutex_);
  return signaled_;
}

}  // namespace xenon::kernel
