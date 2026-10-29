// Signal-time wait satisfaction (NT KiWaitTest semantics).
//
// A signal satisfies the waits already registered on an object, oldest
// first, before it returns; a thread that starts waiting afterwards cannot
// take that signal. Xenon used to set the object and notify, leaving the
// woken waiter to re-check: a thread that signalled and then immediately
// waited took its own signal back. AC6 deadlocked on exactly this: two
// threads doing NtSignalAndWaitForSingleObjectEx(mutant, auto-reset event)
// as a condition variable, where the signalling thread consumed the event
// meant for the one already waiting, leaving both waiting forever.
//
// Each test fails against that old behaviour.

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "xenon/kernel/event.hpp"
#include "xenon/kernel/mutant.hpp"
#include "xenon/kernel/semaphore.hpp"
#include "xenon/kernel/wait.hpp"

using namespace xenon::kernel;
using namespace std::chrono_literals;

namespace {

// Until `count` waits are registered on `object` (a waiter is then blocked
// and entitled to the next signal).
void await_waiters(KernelObject& object, std::size_t count) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (waiter_count(object) < count) {
    assert(std::chrono::steady_clock::now() < deadline && "waiter never registered");
    std::this_thread::yield();
  }
}

WaitResult wait_one(const std::shared_ptr<KernelObject>& object, std::chrono::milliseconds timeout,
                    std::uint32_t thread_id = 0) {
  return wait_for_single_object(object, timeout, thread_id);
}

// The AC6 deadlock's core: the thread that sets an auto-reset event and then
// waits on it must not take the signal from the thread already waiting.
void test_auto_reset_event_goes_to_the_registered_waiter() {
  auto event = std::make_shared<KernelEvent>(false, false);
  std::atomic<int> a_result{-1};
  std::thread a([&] { a_result = static_cast<int>(wait_one(event, -1ms)); });
  await_waiters(*event, 1);

  event->set();
  // Satisfied for A at the signal: the event is unsignalled again at once.
  assert(!event->signaled());
  assert(wait_one(event, 50ms) == WaitResult::Timeout);
  a.join();
  assert(a_result == static_cast<int>(WaitResult::Success));
}

// Each released unit goes to a registered waiter before a later arrival.
void test_semaphore_release_goes_to_registered_waiters_first() {
  auto semaphore = std::make_shared<KernelSemaphore>(0, 10);
  std::atomic<int> released{0};
  std::vector<std::thread> waiters;
  for (int i = 0; i < 2; ++i) {
    waiters.emplace_back([&] {
      if (wait_one(semaphore, -1ms) == WaitResult::Success) ++released;
    });
  }
  await_waiters(*semaphore, 2);
  std::int32_t previous = -1;
  assert(semaphore->release(3, &previous) && previous == 0);
  // Two units went to the two waiters; one is left for a later arrival.
  assert(semaphore->count() == 1);
  assert(wait_one(semaphore, 0ms) == WaitResult::Success);
  assert(wait_one(semaphore, 0ms) == WaitResult::Timeout);
  for (auto& thread : waiters) thread.join();
  assert(released == 2);
}

// Waits are satisfied oldest first: with three registered waiters, each
// single release goes to the one that has waited longest.
void test_releases_go_to_waiters_in_arrival_order() {
  auto semaphore = std::make_shared<KernelSemaphore>(0, 10);
  std::vector<int> order;
  std::mutex order_mutex;
  std::vector<std::thread> waiters;
  for (int i = 0; i < 3; ++i) {
    waiters.emplace_back([&, i] {
      assert(wait_one(semaphore, 10000ms) == WaitResult::Success);
      std::scoped_lock lock(order_mutex);
      order.push_back(i);
    });
    await_waiters(*semaphore, std::size_t(i) + 1u);
  }
  for (int released = 1; released <= 3; ++released) {
    assert(semaphore->release(1));
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (true) {
      {
        std::scoped_lock lock(order_mutex);
        if (order.size() == std::size_t(released)) break;
      }
      assert(std::chrono::steady_clock::now() < deadline);
      std::this_thread::yield();
    }
  }
  for (auto& thread : waiters) thread.join();
  assert((order == std::vector<int>{0, 1, 2}));
}

// A released mutant is owned by the oldest waiter before release() returns;
// the releasing thread cannot take it straight back.
void test_mutant_release_passes_ownership_to_the_waiter() {
  constexpr std::uint32_t kOwner = 21;
  constexpr std::uint32_t kWaiter = 22;
  auto mutant = std::make_shared<KernelMutant>(true, kOwner);
  std::atomic<int> waiter_result{-1};
  std::thread waiter([&] {
    waiter_result = static_cast<int>(wait_one(mutant, -1ms, kWaiter));
    assert(static_cast<KernelMutant&>(*mutant).release(kWaiter));
  });
  await_waiters(*mutant, 1);
  assert(static_cast<KernelMutant&>(*mutant).release(kOwner));
  assert(static_cast<KernelMutant&>(*mutant).owner_thread_id() == kWaiter ||
         waiter_result == static_cast<int>(WaitResult::Success));
  waiter.join();
  assert(waiter_result == static_cast<int>(WaitResult::Success));
  assert(!static_cast<KernelMutant&>(*mutant).is_owned());
}

// Pulse releases exactly the waits registered at that moment and leaves the
// event unsignalled for later arrivals.
void test_pulse_releases_the_registered_waiter_only() {
  auto event = std::make_shared<KernelEvent>(false, false);
  std::atomic<int> a_result{-1};
  std::thread a([&] { a_result = static_cast<int>(wait_one(event, -1ms)); });
  await_waiters(*event, 1);
  event->pulse();
  a.join();
  assert(a_result == static_cast<int>(WaitResult::Success));
  assert(!event->signaled());
  assert(wait_one(event, 20ms) == WaitResult::Timeout);
}

// A WaitAll that cannot be satisfied yet does not hold back an object a
// later WaitAny can take: the signal skips it (and leaves it registered).
void test_unsatisfiable_wait_all_does_not_block_a_later_wait_any() {
  auto first = std::make_shared<KernelEvent>(false, false);
  auto second = std::make_shared<KernelEvent>(false, false);
  std::atomic<int> all_result{-1};
  std::thread all([&] {
    const std::vector<std::shared_ptr<KernelObject>> objects{first, second};
    all_result = static_cast<int>(wait_for_multiple_objects(objects, true, -1ms));
  });
  await_waiters(*first, 1);
  first->set();
  assert(first->signaled());  // the WaitAll could not take it alone
  assert(wait_one(first, 0ms) == WaitResult::Success);
  // Both signalled together satisfy the WaitAll.
  first->set();
  second->set();
  all.join();
  assert(all_result == static_cast<int>(WaitResult::Success));
  assert(!first->signaled() && !second->signaled());
}

// AC6's pattern end to end: a condition variable built from a mutant and an
// auto-reset event, waited on with signal-and-wait by two threads in turn.
// Each round, the thread holding the lock wakes the other (sets the event)
// and then waits itself (releases the lock and waits on the event). Fails if
// a signaller can take its own wake-up back, or if the release and the wait
// are two steps (the other thread's SetEvent then lands before the wait).
void test_condition_variable_ping_pong_does_not_deadlock() {
  auto lock = std::make_shared<KernelMutant>(false, 0);
  auto wake = std::make_shared<KernelEvent>(false, false);
  constexpr int kRounds = 2000;
  std::atomic<int> turn{0};  // whose turn it is: 0 or 1
  std::atomic<bool> stalled{false};

  const auto player = [&](std::uint32_t thread_id, int me) {
    assert(wait_one(lock, -1ms, thread_id) == WaitResult::Success);
    for (int round = 0; round < kRounds; ++round) {
      while (turn.load() != me) {
        // Release the lock and wait to be woken, as one operation:
        // NtSignalAndWaitForSingleObjectEx.
        bool released = false;
        const auto result = signal_and_wait_for_object(
            [&] { return released = static_cast<KernelMutant&>(*lock).release_locked(thread_id); },
            wake, 5000ms, thread_id);
        assert(released);
        if (result == WaitResult::Timeout) {
          stalled = true;
          return;
        }
        assert(result == WaitResult::Success);
        assert(wait_one(lock, -1ms, thread_id) == WaitResult::Success);
      }
      turn = 1 - me;
      wake->set();  // wake the other player, then loop round to wait
    }
    assert(static_cast<KernelMutant&>(*lock).release(thread_id));
    wake->set();
  };
  std::thread a(player, 31u, 0);
  std::thread b(player, 32u, 1);
  a.join();
  b.join();
  assert(!stalled && "a wake-up was taken by the thread that signalled it");
}

// A grant racing the waiter's timeout is never lost: either the wait reports
// success and the signal was consumed for it, or it timed out and the signal
// is still there. Over many races the units are conserved exactly.
void test_a_grant_racing_a_timeout_is_never_lost() {
  auto semaphore = std::make_shared<KernelSemaphore>(0, 1 << 20);
  std::atomic<int> taken{0};
  constexpr int kWaiters = 4;
  constexpr int kReleases = 4000;
  std::atomic<bool> done{false};
  std::vector<std::thread> waiters;
  for (int i = 0; i < kWaiters; ++i) {
    waiters.emplace_back([&] {
      while (!done.load()) {
        if (wait_one(semaphore, 1ms) == WaitResult::Success) ++taken;
      }
    });
  }
  for (int i = 0; i < kReleases; ++i) {
    assert(semaphore->release(1));
    if (i % 64 == 0) std::this_thread::sleep_for(100us);
  }
  // Let the waiters drain what is left, then stop them.
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (semaphore->count() != 0) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(1ms);
  }
  done = true;
  for (auto& thread : waiters) thread.join();
  assert(semaphore->count() == 0);
  assert(taken.load() == kReleases);
}

}  // namespace

int main() {
  test_auto_reset_event_goes_to_the_registered_waiter();
  test_semaphore_release_goes_to_registered_waiters_first();
  test_releases_go_to_waiters_in_arrival_order();
  test_mutant_release_passes_ownership_to_the_waiter();
  test_pulse_releases_the_registered_waiter_only();
  test_unsatisfiable_wait_all_does_not_block_a_later_wait_any();
  test_condition_variable_ping_pong_does_not_deadlock();
  test_a_grant_racing_a_timeout_is_never_lost();
  std::cout << "All wait handoff tests passed!\n";
  return 0;
}
