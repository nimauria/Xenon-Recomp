#include "xenon/kernel/event.hpp"

#include "xenon/kernel/wait.hpp"

namespace xenon::kernel {

KernelEvent::KernelEvent(bool manual_reset, bool initial_state)
    : KernelObject(ObjectType::Event),
      manual_reset_(manual_reset),
      signaled_(initial_state) {}

void KernelEvent::set() {
  std::scoped_lock lock(mutex_);
  set_locked();
}

void KernelEvent::set_locked() {
  signaled_ = true;
  // An auto-reset event goes to the oldest waiter and stays unsignalled; a
  // manual-reset event releases every waiter and stays signalled.
  multi_waiters_.satisfy_waiters_locked();
  store_state_locked();
}

void KernelEvent::pulse() {
  std::scoped_lock lock(mutex_);
  signaled_ = true;
  multi_waiters_.satisfy_waiters_locked();
  signaled_ = false;
  store_state_locked();
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
  KernelObject* const self = this;
  return wait_for_objects(std::span(&self, 1), false, timeout) == WaitResult::Success;
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
  // A guest-written signal is a signal: it satisfies the waits registered
  // here, as the kernel's own would.
  if (signaled_) {
    multi_waiters_.satisfy_waiters_locked();
    if (!signaled_) store_state_locked();
  }
  return GuestStateSync::Adopted;
}

}  // namespace xenon::kernel
