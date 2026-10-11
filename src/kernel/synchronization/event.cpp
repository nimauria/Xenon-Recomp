#include "xenon/kernel/event.hpp"

#include "kernel/synchronization/wait_internal.hpp"
#include "xenon/kernel/wait_util.hpp"

namespace xenon::kernel {

KernelEvent::KernelEvent(bool manual_reset, bool initial_state)
    : KernelObject(ObjectType::Event),
      manual_reset_(manual_reset),
      signaled_(initial_state) {}

void KernelEvent::set() {
  {
    std::scoped_lock lock(mutex_);
    signaled_ = true;
    store_state_locked();
  }
  if (manual_reset_) {
    condition_.notify_all();
  } else {
    condition_.notify_one();
  }
  detail::notify_multi_object_waiters();
}

void KernelEvent::reset() {
  std::scoped_lock lock(mutex_);
  signaled_ = false;
  store_state_locked();
}

bool KernelEvent::signaled() const {
  std::scoped_lock lock(mutex_);
  return signaled_;
}

bool KernelEvent::wait_for(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  if (!wait_until_signaled(condition_, lock, timeout, [&] { return can_satisfy_locked(); })) {
    return false;
  }
  satisfy_locked();
  return true;
}

void KernelEvent::attach_signal_state_mirror(std::unique_ptr<SignalStateMirror> mirror) {
  std::scoped_lock lock(mutex_);
  mirror_ = std::move(mirror);
  store_state_locked();
}

GuestStateSync KernelEvent::reconcile_with_guest_state() {
  std::scoped_lock lock(mutex_);
  if (!mirror_) return GuestStateSync::Unchanged;
  const bool guest_signaled = mirror_->load() != 0;
  if (guest_signaled == signaled_) return GuestStateSync::Unchanged;
  signaled_ = guest_signaled;
  return GuestStateSync::Adopted;
}

}  // namespace xenon::kernel
