#include "xenon/kernel/timer_manager.hpp"

#include <algorithm>

namespace xenon::kernel {

TimerManager::TimerManager() {
  thread_ = std::thread(&TimerManager::dispatch_loop, this);
}

TimerManager::~TimerManager() {
  {
    std::scoped_lock lock(mutex_);
    shutting_down_ = true;
  }
  condition_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void TimerManager::schedule(std::shared_ptr<KernelTimer> timer) {
  {
    std::scoped_lock lock(mutex_);
    const bool already_scheduled =
        std::find(pending_.begin(), pending_.end(), timer) != pending_.end();
    if (!already_scheduled) {
      pending_.push_back(std::move(timer));
    }
  }
  // Always notify: even an already-scheduled timer's due time may have
  // just changed (re-armed via another set() call), and the dispatch
  // thread needs to re-scan to pick up a potentially earlier deadline.
  condition_.notify_all();
}

void TimerManager::unschedule(const std::shared_ptr<KernelTimer>& timer) {
  {
    std::scoped_lock lock(mutex_);
    pending_.erase(std::remove(pending_.begin(), pending_.end(), timer), pending_.end());
  }
  condition_.notify_all();
}

void TimerManager::dispatch_loop() {
  std::unique_lock lock(mutex_);
  while (!shutting_down_) {
    if (pending_.empty()) {
      condition_.wait(lock, [this] { return shutting_down_ || !pending_.empty(); });
      continue;
    }

    std::optional<std::chrono::steady_clock::time_point> earliest_due;
    for (const auto& timer : pending_) {
      const auto due = timer->next_due_time();
      if (due && (!earliest_due || *due < *earliest_due)) {
        earliest_due = due;
      }
    }
    if (!earliest_due) {
      // Every pending timer was cancelled concurrently (next_due_time()
      // returns nullopt once inactive) - nothing left to wait for.
      pending_.clear();
      continue;
    }

    // No predicate here deliberately: any notify_all() (from schedule() or
    // unschedule(), including a re-arm that might have moved the earliest
    // deadline earlier) must wake this loop immediately to re-scan, not
    // only once shutting_down_ becomes true.
    const auto wait_status = condition_.wait_until(lock, *earliest_due);
    if (shutting_down_) {
      break;
    }
    if (wait_status == std::cv_status::no_timeout) {
      continue;  // Woken by schedule()/unschedule() - re-scan from the top.
    }

    // Actually reached a due time: fire every timer that is now due (two
    // timers can legitimately share a deadline).
    const auto now = std::chrono::steady_clock::now();
    for (auto it = pending_.begin(); it != pending_.end();) {
      const auto due = (*it)->next_due_time();
      if (due && *due <= now) {
        const bool rearmed = (*it)->fire();
        if (!rearmed) {
          it = pending_.erase(it);
          continue;
        }
      }
      ++it;
    }
  }
}

}  // namespace xenon::kernel
