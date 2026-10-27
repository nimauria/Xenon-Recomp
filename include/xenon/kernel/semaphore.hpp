#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

#include "xenon/kernel/multi_wait_list.hpp"
#include "xenon/kernel/object.hpp"
#include "xenon/kernel/signal_state_mirror.hpp"

namespace xenon::kernel {

class KernelSemaphore final : public KernelObject {
 public:
  explicit KernelSemaphore(std::int32_t initial_count, std::int32_t maximum_count);

  [[nodiscard]] bool release(std::int32_t release_count, std::int32_t* previous_count = nullptr);
  // As above, for a caller already holding the dispatcher lock
  // (detail::dispatcher_mutex()), such as signal_and_wait_for_object().
  [[nodiscard]] bool release_locked(std::int32_t release_count,
                                    std::int32_t* previous_count = nullptr);
  [[nodiscard]] bool wait_for(std::chrono::milliseconds timeout);
  [[nodiscard]] std::int32_t count() const;
  [[nodiscard]] std::int32_t maximum_count() const noexcept { return maximum_count_; }

  // For a semaphore embedded in guest memory: from now on every count change
  // is stored into `mirror` (SignalState is the count), starting with the
  // current one.
  void attach_signal_state_mirror(std::unique_ptr<SignalStateMirror> mirror);
  // Adopts a count the guest wrote directly since the last store, if it lies
  // within 0 and the limit; otherwise keeps the current count (Rejected).
  [[nodiscard]] GuestStateSync reconcile_with_guest_state();

 private:
  friend struct detail::WaitAccess;
  // Caller holds mutex_. A satisfied wait takes one unit.
  [[nodiscard]] bool can_satisfy_locked() const noexcept { return count_ > 0; }
  void satisfy_locked() noexcept {
    --count_;
    if (mirror_) mirror_->store(count_);
  }

  std::mutex& mutex_ = detail::dispatcher_mutex();
  std::int32_t count_;
  std::int32_t maximum_count_;
  std::unique_ptr<SignalStateMirror> mirror_;
  detail::MultiWaitList multi_waiters_;
};

}  // namespace xenon::kernel
