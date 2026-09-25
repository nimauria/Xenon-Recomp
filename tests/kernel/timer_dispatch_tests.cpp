// Phase 3 (AC6 Runtime Readiness pass): real timer-dispatch thread.
//
// Regression coverage for the real bug the audit found: KernelTimer::set()
// only ever signaled a timer immediately when due_time == 0; nothing
// anywhere fired a timer with a real future due time, so any guest code
// that set a delayed or periodic kernel timer and waited on it would hang
// forever. kernel::TimerManager (owned per-process via
// KernelProcess::timer_manager()) now actually fires them.

#include "xenon/kernel/timer_manager.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

using namespace xenon::kernel;
using namespace std::chrono_literals;

namespace {

void test_timer_with_nonzero_due_time_actually_fires() {
  TimerManager manager;
  auto timer = std::make_shared<KernelTimer>(TimerType::NotificationTimer);

  assert(!timer->is_signaled());
  assert(timer->set(50ms));
  manager.schedule(timer);

  // wait_for() must actually observe the timer becoming signaled once the
  // dispatch thread fires it - not the old behavior where a nonzero
  // due_time never signaled at all.
  assert(timer->wait_for(2000ms) && "a scheduled timer with due_time > 0 must actually fire");
  assert(timer->is_signaled());
}

void test_timer_fires_at_approximately_the_requested_time() {
  TimerManager manager;
  auto timer = std::make_shared<KernelTimer>(TimerType::NotificationTimer);

  constexpr auto kDelay = 150ms;
  const auto start = std::chrono::steady_clock::now();
  assert(timer->set(kDelay));
  manager.schedule(timer);

  assert(timer->wait_for(2000ms));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  assert(elapsed >= kDelay - 20ms && "must not fire meaningfully early");
  assert(elapsed < kDelay + 500ms && "must not fire only after a long, unrelated delay");
}

void test_periodic_timer_refires() {
  TimerManager manager;
  auto timer = std::make_shared<KernelTimer>(TimerType::SynchronizationTimer);

  std::atomic<int> fire_count{0};
  assert(timer->set(30ms, 30ms, [&] { fire_count.fetch_add(1); }));
  manager.schedule(timer);

  const auto start = std::chrono::steady_clock::now();
  while (fire_count.load() < 3 &&
         std::chrono::steady_clock::now() - start < 3000ms) {
    std::this_thread::sleep_for(10ms);
  }

  assert(fire_count.load() >= 3 && "a periodic timer must re-fire more than once");
}

void test_cancel_before_due_time_prevents_firing() {
  TimerManager manager;
  auto timer = std::make_shared<KernelTimer>(TimerType::NotificationTimer);

  assert(timer->set(200ms));
  manager.schedule(timer);
  std::this_thread::sleep_for(20ms);
  assert(timer->cancel());
  manager.unschedule(timer);

  // Give the original due time a chance to pass; it must not have fired.
  std::this_thread::sleep_for(300ms);
  assert(!timer->is_signaled() && "a cancelled timer must not fire");
}

void test_manager_shutdown_does_not_hang_with_pending_timers() {
  auto manager = std::make_unique<TimerManager>();
  auto timer = std::make_shared<KernelTimer>(TimerType::NotificationTimer);
  assert(timer->set(10min));  // far in the future - should never actually fire in this test
  manager->schedule(timer);

  const auto start = std::chrono::steady_clock::now();
  manager.reset();  // destructor must join its dispatch thread promptly
  const auto elapsed = std::chrono::steady_clock::now() - start;
  assert(elapsed < 2000ms && "TimerManager teardown must not block on a far-future pending timer");
}

}  // namespace

int main() {
  std::cout << "Testing kernel timer dispatch...\n";

  test_timer_with_nonzero_due_time_actually_fires();
  test_timer_fires_at_approximately_the_requested_time();
  test_periodic_timer_refires();
  test_cancel_before_due_time_prevents_firing();
  test_manager_shutdown_does_not_hang_with_pending_timers();

  std::cout << "All kernel timer dispatch tests passed!\n";
  return 0;
}
