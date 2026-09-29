// Regression test: an infinite wait must actually wait.
//
// "Wait forever" is encoded as a negative timeout (xbox_infinite_timeout(), and
// what a NULL timeout pointer on KeWaitForSingleObject/NtWaitForSingleObjectEx
// becomes). std::condition_variable::wait_for treats a negative duration as
// already expired, so passing the sentinel through unchanged made every infinite
// wait on an event, semaphore, mutant or timer return STATUS_TIMEOUT
// immediately - a worker thread's "wait for work" loop would spin instead of
// sleeping, and the critical-section slow path never blocked.
//
// Each waitable object type is waited on with the infinite sentinel from a real
// host thread: the waiter must still be blocked after a delay, then wake and
// report success once the object is signaled.

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "xenon/kernel/event.hpp"
#include "xenon/kernel/mutant.hpp"
#include "xenon/kernel/semaphore.hpp"
#include "xenon/kernel/timer.hpp"
#include "xenon/kernel/wait.hpp"

using namespace xenon::kernel;
using namespace std::chrono_literals;

namespace {

constexpr std::chrono::milliseconds kInfinite{-1};

// Runs `signal` after the waiter has demonstrably been blocked for a while.
template <class Object, class Signal>
void expect_blocks_then_wakes(const std::shared_ptr<Object>& object, Signal signal) {
  std::atomic<bool> returned{false};
  WaitResult result = WaitResult::Failed;
  std::thread waiter([&] {
    result = wait_for_single_object(object, kInfinite, /*waiting_thread_id=*/7);
    returned = true;
  });
  std::this_thread::sleep_for(150ms);
  assert(!returned.load() && "an infinite wait must not return before the object is signaled");
  signal();
  waiter.join();
  assert(result == WaitResult::Success);
}

}  // namespace

// (The signal callbacks need the objects; each test builds its own pair.)
namespace {

void test_manual_event() {
  auto event = std::make_shared<KernelEvent>(true, false);
  expect_blocks_then_wakes(event, [&] { event->set(); });
  assert(event->signaled() && "a manual-reset event stays signaled");
}

void test_auto_event() {
  auto event = std::make_shared<KernelEvent>(false, false);
  expect_blocks_then_wakes(event, [&] { event->set(); });
  assert(!event->signaled() && "an auto-reset event is consumed by the waiter");
}

void test_semaphore() {
  auto semaphore = std::make_shared<KernelSemaphore>(0, 10);
  expect_blocks_then_wakes(semaphore, [&] { assert(semaphore->release(1)); });
  assert(semaphore->count() == 0);
}

void test_mutant() {
  // Owned by thread 3; a different thread's infinite acquire blocks until release.
  auto mutant = std::make_shared<KernelMutant>(/*initial_owner=*/true, /*owner_thread_id=*/3);
  expect_blocks_then_wakes(mutant, [&] { assert(mutant->release(3)); });
  assert(mutant->owner_thread_id() == 7u);
}

void test_timer() {
  auto timer = std::make_shared<KernelTimer>(TimerType::SynchronizationTimer);
  std::atomic<bool> returned{false};
  WaitResult result = WaitResult::Failed;
  std::thread waiter([&] {
    result = wait_for_single_object(timer, kInfinite, 7);
    returned = true;
  });
  std::this_thread::sleep_for(150ms);
  assert(!returned.load());
  // due_time 0 fires the timer inline and wakes the waiter.
  assert(timer->set(0ms));
  waiter.join();
  assert(result == WaitResult::Success);
}

// A finite timeout still times out, and an already-signaled object is acquired
// without blocking regardless of the timeout value.
void test_finite_and_zero_timeouts_unchanged() {
  auto event = std::make_shared<KernelEvent>(true, false);
  const auto start = std::chrono::steady_clock::now();
  assert(wait_for_single_object(event, 60ms, 1) == WaitResult::Timeout);
  assert(std::chrono::steady_clock::now() - start >= 50ms && "a finite timeout waits its duration");
  assert(wait_for_single_object(event, 0ms, 1) == WaitResult::Timeout && "zero polls");
  event->set();
  assert(wait_for_single_object(event, kInfinite, 1) == WaitResult::Success &&
         "an already-signaled object needs no blocking");
  assert(wait_for_single_object(event, 0ms, 1) == WaitResult::Success);
}

}  // namespace

int main() {
  std::cout << "Testing infinite kernel waits...\n";
  test_manual_event();
  test_auto_event();
  test_semaphore();
  test_mutant();
  test_timer();
  test_finite_and_zero_timeouts_unchanged();
  std::cout << "All infinite wait tests passed!\n";
  return 0;
}
