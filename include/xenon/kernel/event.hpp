#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "xenon/kernel/multi_wait_list.hpp"
#include "xenon/kernel/object.hpp"
#include "xenon/kernel/signal_state_mirror.hpp"

namespace xenon::kernel {

class KernelEvent final : public KernelObject {
 public:
  explicit KernelEvent(bool manual_reset = false, bool initial_state = false);

  void set();
  // As above, for a caller already holding the dispatcher lock
  // (detail::dispatcher_mutex()), such as signal_and_wait_for_object().
  void set_locked();
  void reset();
  // Releases the waits it can satisfy now (every one for a manual-reset
  // event, the oldest for an auto-reset one) and leaves the event
  // unsignalled (NtPulseEvent/KePulseEvent).
  void pulse();
  [[nodiscard]] bool signaled() const;
  // True for a notification (manual-reset) event, false for a synchronization
  // (auto-reset) event - EVENT_BASIC_INFORMATION's EventType is the inverse.
  [[nodiscard]] bool manual_reset() const noexcept { return manual_reset_; }
  [[nodiscard]] bool wait_for(std::chrono::milliseconds timeout);

  // For an event embedded in guest memory: from now on every state change is
  // stored into `mirror` (SignalState 1 or 0), starting with the current one.
  void attach_signal_state_mirror(std::unique_ptr<SignalStateMirror> mirror);
  // Adopts a SignalState the guest wrote directly since the last store; any
  // nonzero value means signalled.
  [[nodiscard]] GuestStateSync reconcile_with_guest_state();

 private:
  friend struct detail::WaitAccess;
  // Caller holds the dispatcher lock (mutex_). A satisfied wait consumes an
  // auto-reset event.
  [[nodiscard]] bool can_satisfy_locked() const noexcept { return signaled_; }
  void satisfy_locked() noexcept {
    if (manual_reset_) return;
    signaled_ = false;
    if (mirror_) mirror_->store(0);
  }
  void store_state_locked() noexcept {
    if (mirror_) mirror_->store(signaled_ ? 1 : 0);
  }

  bool manual_reset_{};
  std::mutex& mutex_ = detail::dispatcher_mutex();
  bool signaled_{};
  std::unique_ptr<SignalStateMirror> mirror_;
  detail::MultiWaitList multi_waiters_;
};

}  // namespace xenon::kernel
