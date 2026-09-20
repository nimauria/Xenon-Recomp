#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

#include "xenon/kernel/object.hpp"

namespace xenon::kernel {

enum class TimerType {
  NotificationTimer,  // Manual reset
  SynchronizationTimer,  // Auto reset
};

using TimerCallback = std::function<void()>;

class KernelTimer final : public KernelObject {
 public:
  explicit KernelTimer(TimerType type);

  [[nodiscard]] bool set(std::chrono::milliseconds due_time,
                         std::optional<std::chrono::milliseconds> period = std::nullopt,
                         TimerCallback callback = nullptr);
  [[nodiscard]] bool cancel();
  [[nodiscard]] bool wait_for(std::chrono::milliseconds timeout);
  [[nodiscard]] bool is_signaled() const;

 private:
  TimerType type_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool signaled_{false};
  bool active_{false};
  std::chrono::steady_clock::time_point due_time_;
  std::optional<std::chrono::milliseconds> period_;
  TimerCallback callback_;
};

}  // namespace xenon::kernel
