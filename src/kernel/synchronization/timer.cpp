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

  // due_time == 0 fires immediately and synchronously here. A nonzero
  // due_time only arms due_time_/period_ above - the caller (the
  // NtSetTimerEx export handler) is responsible for registering this timer
  // with a kernel::TimerManager (see next_due_time()/fire()) so it actually
  // fires later; KernelTimer itself owns no timer thread.
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

std::optional<std::chrono::steady_clock::time_point> KernelTimer::next_due_time() const {
  std::scoped_lock lock(mutex_);
  if (!active_) {
    return std::nullopt;
  }
  return due_time_;
}

bool KernelTimer::fire() {
  TimerCallback callback_copy;
  bool rearmed = false;
  {
    std::scoped_lock lock(mutex_);
    if (!active_) {
      return false;  // Cancelled concurrently before TimerManager got here.
    }

    signaled_ = true;
    condition_.notify_all();
    callback_copy = callback_;

    if (period_) {
      due_time_ = std::chrono::steady_clock::now() + *period_;
      rearmed = true;
    } else {
      active_ = false;
    }
  }

  // Invoked outside the lock: a callback that touches this same KernelTimer
  // (e.g. re-arming it) must not deadlock against mutex_.
  if (callback_copy) {
    callback_copy();
  }
  return rearmed;
}

}  // namespace xenon::kernel
