#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

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

  [[nodiscard]] bool acquire(std::uint32_t owner_thread_id, std::chrono::milliseconds timeout);
  [[nodiscard]] bool release(std::uint32_t owner_thread_id);
  [[nodiscard]] bool is_owned() const;
  [[nodiscard]] std::uint32_t owner_thread_id() const;
  [[nodiscard]] std::int32_t recursion_count() const;

  // Called when owning thread terminates
  void abandon();

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::uint32_t owner_thread_id_{0};
  std::int32_t recursion_count_{0};
  bool abandoned_{false};
};

}  // namespace xenon::kernel
