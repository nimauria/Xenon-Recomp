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

  // due_time == 0 fires synchronously, inline, before set() returns
  // (unchanged existing behavior). due_time > 0 only arms due_time_/period_
  // here - actually firing it later requires a TimerManager
  // (kernel::KernelProcess::timer_manager()) to have this timer scheduled
  // via schedule(); set() itself has no timer thread of its own (see
  // next_due_time()/fire() below).
  [[nodiscard]] bool set(std::chrono::milliseconds due_time,
                         std::optional<std::chrono::milliseconds> period = std::nullopt,
                         TimerCallback callback = nullptr);
  [[nodiscard]] bool cancel();
  [[nodiscard]] bool wait_for(std::chrono::milliseconds timeout);
  [[nodiscard]] bool is_signaled() const;

  // Absolute due time if currently active (set() was called and cancel()
  // has not been since), else std::nullopt. Used by TimerManager's dispatch
  // thread to know when to wake and fire this timer - not part of the
  // guest-facing API surface.
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_due_time() const;

  // Fires this timer: marks it signaled, wakes wait_for() waiters, invokes
  // its host callback (if any - see set()'s callback parameter; a guest
  // callback ROUTINE from NtSetTimerEx is a distinct, not-yet-modeled
  // concept, see docs/kernel/THREADING_V2.md), and re-arms due_time_ by
  // period_ for a periodic timer. Only TimerManager's dispatch thread calls
  // this, at the timer's actual due time. Returns true if the timer
  // re-armed and should stay scheduled, false if it fired once (or was
  // concurrently cancelled) and should be unscheduled.
  bool fire();

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
