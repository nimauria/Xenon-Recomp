#include "xenon/kernel/event.hpp"

namespace xenon::kernel {

KernelEvent::KernelEvent(bool manual_reset, bool initial_state)
    : KernelObject(ObjectType::Event),
      manual_reset_(manual_reset),
      signaled_(initial_state) {}

void KernelEvent::set() {
  {
    std::scoped_lock lock(mutex_);
    signaled_ = true;
  }
  if (manual_reset_) {
    condition_.notify_all();
  } else {
    condition_.notify_one();
  }
}

void KernelEvent::reset() {
  std::scoped_lock lock(mutex_);
  signaled_ = false;
}

bool KernelEvent::signaled() const {
  std::scoped_lock lock(mutex_);
  return signaled_;
}

bool KernelEvent::wait_for(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  if (!condition_.wait_for(lock, timeout, [&] { return signaled_; })) {
    return false;
  }
  if (!manual_reset_) signaled_ = false;
  return true;
}

}  // namespace xenon::kernel
