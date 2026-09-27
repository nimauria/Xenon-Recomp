#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "xenon/kernel/object.hpp"

namespace xenon::kernel {

class KernelSemaphore final : public KernelObject {
 public:
  explicit KernelSemaphore(std::int32_t initial_count, std::int32_t maximum_count);

  [[nodiscard]] bool release(std::int32_t release_count, std::int32_t* previous_count = nullptr);
  [[nodiscard]] bool wait_for(std::chrono::milliseconds timeout);
  [[nodiscard]] std::int32_t count() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::int32_t count_;
  std::int32_t maximum_count_;
};

}  // namespace xenon::kernel
