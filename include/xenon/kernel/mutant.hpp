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
  explicit KernelMutant(bool initial_owner = false);

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
