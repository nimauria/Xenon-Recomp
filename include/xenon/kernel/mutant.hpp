#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

#include "xenon/kernel/multi_wait_list.hpp"
#include "xenon/kernel/object.hpp"

namespace xenon::kernel {

// Xbox mutant (similar to Windows mutex but with abandon semantics)
class KernelMutant final : public KernelObject {
 public:
  // owner_thread_id identifies the creating thread and is only meaningful
  // when initial_owner is true; the caller (the NtCreateMutant export
  // handler) must pass the real creating thread's id - there is no safe
  // default, since guest thread ids start at 1 (see ThreadManager) and a
  // hardcoded placeholder would make a fresh mutant appear pre-owned by an
  // unrelated thread.
  explicit KernelMutant(bool initial_owner = false, std::uint32_t owner_thread_id = 0);
  ~KernelMutant() override;

  // abandoned, when given, reports whether this acquisition took over a
  // mutant whose previous owner exited without releasing it (the wait then
  // returns STATUS_ABANDONED rather than STATUS_SUCCESS).
  [[nodiscard]] bool acquire(std::uint32_t owner_thread_id, std::chrono::milliseconds timeout,
                             bool* abandoned = nullptr);
  [[nodiscard]] bool release(std::uint32_t owner_thread_id);
  // As above, for a caller already holding the dispatcher lock
  // (detail::dispatcher_mutex()), such as signal_and_wait_for_object().
  [[nodiscard]] bool release_locked(std::uint32_t owner_thread_id);
  [[nodiscard]] bool is_owned() const;
  [[nodiscard]] std::uint32_t owner_thread_id() const;
  [[nodiscard]] std::int32_t recursion_count() const;

  // Drops ownership without the owner's release; the next acquisition
  // reports the mutant abandoned. The kernel calls this for every mutant a
  // thread still owns when the thread exits (abandon_mutants_owned_by_this_thread()).
  void abandon();

 private:
  friend struct detail::WaitAccess;
  friend void abandon_mutants_owned_by_this_thread() noexcept;

  // Caller holds mutex_. Free, or already owned by the waiting thread
  // (recursive acquisition); a satisfied wait takes or deepens ownership.
  [[nodiscard]] bool can_satisfy_locked(std::uint32_t thread_id) const noexcept {
    return recursion_count_ == 0 || owner_thread_id_ == thread_id;
  }
  // Returns whether this took over an abandoned mutant. Ownership is also
  // recorded against the waiting thread's host thread, which is the guest
  // thread's own: every guest thread runs on its own host thread, and a
  // thread's exit abandons what that host thread still owns. A signaller
  // satisfying the wait for a waiter passes the waiter's host thread.
  bool satisfy_locked(std::uint32_t thread_id, std::thread::id host_thread) noexcept {
    if (recursion_count_ > 0) {
      ++recursion_count_;
      return false;
    }
    owner_thread_id_ = thread_id;
    owner_host_thread_ = host_thread;
    recursion_count_ = 1;
    return std::exchange(abandoned_, false);
  }
  void abandon_locked() noexcept;

  std::mutex& mutex_ = detail::dispatcher_mutex();
  std::uint32_t owner_thread_id_{0};
  std::thread::id owner_host_thread_{};
  std::int32_t recursion_count_{0};
  bool abandoned_{false};
  detail::MultiWaitList multi_waiters_;
};

// Abandons every mutant the calling host thread still owns. Called by a guest
// thread's host thread as it exits, before the thread object is signalled,
// so a waiter woken by the exit already finds the mutants abandoned.
void abandon_mutants_owned_by_this_thread() noexcept;

}  // namespace xenon::kernel
