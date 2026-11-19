#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace xenon::core {

// Wakes a worker thread for one of up to 32 reasons, kept as a bit set so
// none is lost: a reason notified while the worker is busy is returned by its
// next wait. Counts each reason and each wait that timed out, for the stop
// report (which source woke the worker, and how often it polled).
class WakeSignal {
 public:
  static constexpr std::size_t kMaxReasons = 32;

  void notify(std::uint32_t reasons) {
    {
      std::scoped_lock lock(mutex_);
      pending_ |= reasons;
    }
    condition_.notify_one();
  }

  // Waits until a reason is pending or `deadline` passes; returns the
  // pending reasons and clears them (0 when the wait timed out).
  [[nodiscard]] std::uint32_t wait_until(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mutex_);
    condition_.wait_until(lock, deadline, [&] { return pending_ != 0u; });
    const auto reasons = pending_;
    pending_ = 0u;
    lock.unlock();
    if (reasons == 0u) {
      timeouts_.fetch_add(1, std::memory_order_relaxed);
    } else {
      for (auto bits = reasons; bits != 0u; bits &= bits - 1u) {
        counts_[std::countr_zero(bits)].fetch_add(1, std::memory_order_relaxed);
      }
    }
    return reasons;
  }

  // Waits that returned with `reason` pending.
  [[nodiscard]] std::uint64_t count(std::uint32_t reason) const noexcept {
    return reason ? counts_[std::countr_zero(reason)].load(std::memory_order_relaxed) : 0u;
  }
  [[nodiscard]] std::uint64_t timeouts() const noexcept {
    return timeouts_.load(std::memory_order_relaxed);
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::uint32_t pending_{0};
  std::array<std::atomic<std::uint64_t>, kMaxReasons> counts_{};
  std::atomic<std::uint64_t> timeouts_{0};
};

// A monotonically advancing sequence one thread publishes and another waits
// for: "everything up to request N has been done".
class SequenceLatch {
 public:
  void publish(std::uint64_t value) {
    {
      std::scoped_lock lock(mutex_);
      if (value <= value_) return;
      value_ = value;
    }
    condition_.notify_all();
  }
  // Waits until `value` has been published or `deadline` passes; whether it was.
  [[nodiscard]] bool wait_until(std::uint64_t value,
                                std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mutex_);
    return condition_.wait_until(lock, deadline, [&] { return value_ >= value; });
  }
  [[nodiscard]] std::uint64_t value() const {
    std::scoped_lock lock(mutex_);
    return value_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::uint64_t value_{0};
};

}  // namespace xenon::core
