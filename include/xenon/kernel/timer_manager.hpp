#pragma once

// Real timer-dispatch thread (Phase 3 of the AC6 Runtime Readiness pass).
// Previously, KernelTimer::set() only ever signaled a timer immediately
// when due_time == 0; nothing anywhere fired a timer with a real future due
// time, so any guest code that set a periodic or delayed kernel timer and
// waited on it would hang forever. One TimerManager per KernelProcess (see
// KernelProcess::timer_manager()) owns a single dedicated host thread that
// sleeps until the earliest due time among everything scheduled here, fires
// due timers (KernelTimer::fire()), and re-arms periodic ones automatically.

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "xenon/kernel/timer.hpp"

namespace xenon::kernel {

class TimerManager {
 public:
  TimerManager();
  ~TimerManager();

  TimerManager(const TimerManager&) = delete;
  TimerManager& operator=(const TimerManager&) = delete;

  // Registers timer to be dispatched at its own due time (see
  // KernelTimer::next_due_time()) - the caller must already have called
  // timer->set(due_time, ...) with due_time > 0 (a due_time == 0 timer
  // fires synchronously inside set() and does not need scheduling here).
  // Safe to call again for a timer already scheduled (e.g. NtSetTimerEx
  // called a second time on the same handle to re-arm it).
  void schedule(std::shared_ptr<KernelTimer> timer);

  // Removes timer from the pending schedule (e.g. NtCancelTimer). Safe to
  // call for a timer that already fired once (and was not periodic) or was
  // never scheduled.
  void unschedule(const std::shared_ptr<KernelTimer>& timer);

 private:
  void dispatch_loop();

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<std::shared_ptr<KernelTimer>> pending_;
  bool shutting_down_{false};
  std::thread thread_;
};

}  // namespace xenon::kernel
