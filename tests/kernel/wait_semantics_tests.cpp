// Multi-object wait semantics of kernel::wait_for_multiple_objects(), which
// backs KeWaitForMultipleObjects and NtWaitForMultipleObjectsEx.
//
// Contract (NT dispatcher semantics):
// - WaitAll is all-or-nothing: no object is consumed (auto-reset event reset,
//   semaphore decremented, mutant acquired) unless every object can be
//   satisfied at the same moment. A wait that times out consumes nothing.
// - WaitAny satisfies exactly one object, the lowest-indexed satisfiable one,
//   and reports that index.
// - A zero timeout checks once: already-signalled objects succeed, otherwise
//   the wait times out immediately.
// - Manual-reset events and terminated threads satisfy a wait without being
//   consumed.

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "xenon/kernel/event.hpp"
#include "xenon/kernel/mutant.hpp"
#include "xenon/kernel/semaphore.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/kernel/timer.hpp"
#include "xenon/kernel/wait.hpp"

using namespace xenon::kernel;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kThreadA = 7;
constexpr std::uint32_t kThreadB = 8;

WaitResult wait_all(const std::vector<std::shared_ptr<KernelObject>>& objects,
                    std::chrono::milliseconds timeout, std::uint32_t thread = kThreadA) {
  return wait_for_multiple_objects(objects, true, timeout, nullptr, thread);
}

WaitResult wait_any(const std::vector<std::shared_ptr<KernelObject>>& objects,
                    std::chrono::milliseconds timeout, std::uint32_t& index,
                    std::uint32_t thread = kThreadA) {
  return wait_for_multiple_objects(objects, false, timeout, &index, thread);
}

void test_failed_wait_all_consumes_no_event_or_semaphore() {
  auto event = std::make_shared<KernelEvent>(false, true);  // auto-reset, signalled
  auto semaphore = std::make_shared<KernelSemaphore>(1, 10);
  auto blocker = std::make_shared<KernelEvent>(false, false);
  assert(wait_all({event, semaphore, blocker}, 30ms) == WaitResult::Timeout);
  assert(event->signaled() && "a timed-out WaitAll must not reset an auto-reset event");
  assert(semaphore->count() == 1 && "a timed-out WaitAll must not decrement a semaphore");
}

void test_failed_wait_all_acquires_no_mutant() {
  auto mutant = std::make_shared<KernelMutant>();
  auto blocker = std::make_shared<KernelEvent>(false, false);
  assert(wait_all({mutant, blocker}, 30ms) == WaitResult::Timeout);
  assert(!mutant->is_owned() && "a timed-out WaitAll must not leave a mutant owned");
  assert(mutant->acquire(kThreadB, 0ms) && "another thread can still take the mutant");
}

void test_zero_timeout_wait_all_succeeds_when_already_signalled() {
  auto auto_event = std::make_shared<KernelEvent>(false, true);
  auto manual_event = std::make_shared<KernelEvent>(true, true);
  auto semaphore = std::make_shared<KernelSemaphore>(1, 10);
  auto mutant = std::make_shared<KernelMutant>();
  assert(wait_all({auto_event, manual_event, semaphore, mutant}, 0ms) == WaitResult::Success);
  assert(!auto_event->signaled() && "an auto-reset event is consumed by a satisfied wait");
  assert(manual_event->signaled() && "a manual-reset event stays signalled");
  assert(semaphore->count() == 0);
  assert(mutant->is_owned() && mutant->owner_thread_id() == kThreadA);
}

void test_zero_timeout_wait_all_times_out_without_consuming() {
  auto event = std::make_shared<KernelEvent>(false, true);
  auto semaphore = std::make_shared<KernelSemaphore>(0, 10);
  const auto start = std::chrono::steady_clock::now();
  assert(wait_all({event, semaphore}, 0ms) == WaitResult::Timeout);
  assert(std::chrono::steady_clock::now() - start < 500ms && "a zero timeout must not block");
  assert(event->signaled());
}

void test_wait_all_completes_atomically_when_the_last_object_arrives() {
  auto event = std::make_shared<KernelEvent>(false, true);
  auto semaphore = std::make_shared<KernelSemaphore>(0, 10);
  std::atomic<WaitResult> result{WaitResult::Failed};
  std::thread waiter([&] { result = wait_all({event, semaphore}, 5000ms); });
  std::this_thread::sleep_for(50ms);
  assert(event->signaled() && "nothing is consumed while the wait is still incomplete");
  assert(semaphore->release(1));
  waiter.join();
  assert(result.load() == WaitResult::Success);
  assert(!event->signaled() && semaphore->count() == 0);
}

void test_wait_any_reports_and_consumes_only_the_lowest_signalled_index() {
  auto first = std::make_shared<KernelEvent>(false, false);
  auto second = std::make_shared<KernelEvent>(false, true);
  auto third = std::make_shared<KernelEvent>(false, true);
  std::uint32_t index = 99;
  assert(wait_any({first, second, third}, 0ms, index) == WaitResult::Success);
  assert(index == 1);
  assert(!second->signaled() && third->signaled() && "only the reported object is consumed");
  assert(wait_any({first, second, third}, 0ms, index) == WaitResult::Success && index == 2);
  const auto start = std::chrono::steady_clock::now();
  assert(wait_any({first, second, third}, 0ms, index) == WaitResult::Timeout);
  assert(std::chrono::steady_clock::now() - start < 500ms);
}

void test_wait_any_wakes_on_a_later_signal() {
  auto first = std::make_shared<KernelEvent>(false, false);
  auto second = std::make_shared<KernelSemaphore>(0, 10);
  std::uint32_t index = 99;
  std::atomic<WaitResult> result{WaitResult::Failed};
  std::thread waiter([&] { result = wait_any({first, second}, 5000ms, index); });
  std::this_thread::sleep_for(30ms);
  assert(second->release(1));
  waiter.join();
  assert(result.load() == WaitResult::Success && index == 1 && second->count() == 0);
}

void test_wait_all_times_out_on_schedule() {
  auto event = std::make_shared<KernelEvent>(false, false);
  const auto start = std::chrono::steady_clock::now();
  assert(wait_all({event}, 50ms) == WaitResult::Timeout);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  assert(elapsed >= 45ms && elapsed < 2000ms);
}

// Producers release two semaphores together; consumers WaitAll on both.
// Every satisfied wait takes exactly one unit from each, so the two counts
// can never diverge, and every unit released is consumed exactly once.
void test_wait_all_conserves_signals_under_contention() {
  auto left = std::make_shared<KernelSemaphore>(0, 1 << 20);
  auto right = std::make_shared<KernelSemaphore>(0, 1 << 20);
  constexpr int kUnits = 2000;
  constexpr int kConsumers = 4;
  std::atomic<int> consumed{0};
  // A lost signal would leave `consumed` short forever; give up at a deadline
  // so the failure is an assertion, not a hang.
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  std::vector<std::thread> consumers;
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&, c] {
      while (consumed.load() < kUnits && std::chrono::steady_clock::now() < deadline) {
        if (wait_all({left, right}, 20ms, 100u + static_cast<std::uint32_t>(c)) ==
            WaitResult::Success)
          ++consumed;
      }
    });
  }
  std::thread interferer([&] {
    // Takes single units from `left` alone; a non-atomic WaitAll would let
    // consumers strand units of `right` behind these thefts.
    for (int i = 0; i < kUnits / 4; ++i) {
      if (left->wait_for(1ms)) assert(left->release(1));
    }
  });
  for (int i = 0; i < kUnits; ++i) {
    assert(left->release(1));
    assert(right->release(1));
  }
  interferer.join();
  for (auto& consumer : consumers) consumer.join();
  assert(consumed.load() == kUnits);
  assert(left->count() == 0 && right->count() == 0 &&
         "every released unit was consumed exactly once, from both semaphores");
}

// Two consumers WaitAll on the same pair of semaphores listed in opposite
// orders. Objects are locked in one global order, so neither can hold one
// object while waiting for the other.
void test_overlapping_wait_all_in_opposite_orders_does_not_deadlock() {
  auto first = std::make_shared<KernelSemaphore>(0, 1 << 20);
  auto second = std::make_shared<KernelSemaphore>(0, 1 << 20);
  constexpr int kUnits = 1000;
  std::atomic<int> consumed{0};
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  auto consume = [&](std::vector<std::shared_ptr<KernelObject>> pair, std::uint32_t thread) {
    while (consumed.load() < kUnits && std::chrono::steady_clock::now() < deadline) {
      if (wait_all(pair, 20ms, thread) == WaitResult::Success) ++consumed;
    }
  };
  std::thread forward(consume, std::vector<std::shared_ptr<KernelObject>>{first, second}, 200u);
  std::thread backward(consume, std::vector<std::shared_ptr<KernelObject>>{second, first}, 201u);
  for (int i = 0; i < kUnits; ++i) {
    assert(first->release(1));
    assert(second->release(1));
  }
  forward.join();
  backward.join();
  assert(consumed.load() == kUnits);
  assert(first->count() == 0 && second->count() == 0);
}

void test_wait_all_deepens_a_mutant_the_waiter_already_owns() {
  auto mutant = std::make_shared<KernelMutant>(true, kThreadA);
  auto event = std::make_shared<KernelEvent>(false, true);
  assert(wait_all({mutant, event}, 0ms, kThreadA) == WaitResult::Success);
  assert(mutant->owner_thread_id() == kThreadA && mutant->recursion_count() == 2);
  assert(wait_all({mutant, event}, 0ms, kThreadB) == WaitResult::Timeout &&
         "another thread cannot take a mutant that is owned");
}

void test_wait_any_wakes_when_a_thread_terminates() {
  std::atomic<bool> release_worker{false};
  ThreadCreationParams params{};
  auto worker = std::make_shared<KernelThread>(
      [&]() -> std::uint32_t {
        while (!release_worker.load()) std::this_thread::sleep_for(1ms);
        return 0;
      },
      params);
  assert(worker->start());
  auto never = std::make_shared<KernelEvent>(true, false);
  std::uint32_t index = 99;
  std::atomic<WaitResult> result{WaitResult::Failed};
  std::thread waiter([&] { result = wait_any({never, worker}, 5000ms, index); });
  std::this_thread::sleep_for(30ms);
  const auto released = std::chrono::steady_clock::now();
  release_worker = true;
  waiter.join();
  assert(result.load() == WaitResult::Success && index == 1);
  assert(std::chrono::steady_clock::now() - released < 2000ms &&
         "termination wakes the wait; it is not found by the final check at its timeout");
  assert(worker->join());
}

// Every operation that makes an object satisfiable must wake a multi-object
// wait promptly, not leave it to be found by the final check at its timeout.
void test_each_signal_wakes_wait_any_promptly() {
  auto event = std::make_shared<KernelEvent>(false, false);
  auto semaphore = std::make_shared<KernelSemaphore>(0, 10);
  auto released_mutant = std::make_shared<KernelMutant>(true, kThreadB);
  auto abandoned_mutant = std::make_shared<KernelMutant>(true, kThreadB);
  auto timer = std::make_shared<KernelTimer>(TimerType::NotificationTimer);
  const std::vector<std::pair<std::shared_ptr<KernelObject>, std::function<void()>>> cases = {
      {event, [&] { event->set(); }},
      {semaphore, [&] { assert(semaphore->release(1)); }},
      {released_mutant, [&] { assert(released_mutant->release(kThreadB)); }},
      {abandoned_mutant, [&] { abandoned_mutant->abandon(); }},
      {timer, [&] { assert(timer->set(0ms)); }},
  };
  for (const auto& [object, signal] : cases) {
    auto never = std::make_shared<KernelEvent>(true, false);
    std::uint32_t index = 99;
    std::atomic<WaitResult> result{WaitResult::Failed};
    std::thread waiter([&] { result = wait_any({never, object}, 5000ms, index); });
    std::this_thread::sleep_for(30ms);
    const auto signalled = std::chrono::steady_clock::now();
    signal();
    waiter.join();
    assert(result.load() == WaitResult::Success && index == 1);
    assert(std::chrono::steady_clock::now() - signalled < 2000ms);
  }
}

}  // namespace

int main() {
  std::cout << "Testing multi-object wait semantics...\n";
  test_failed_wait_all_consumes_no_event_or_semaphore();
  test_failed_wait_all_acquires_no_mutant();
  test_zero_timeout_wait_all_succeeds_when_already_signalled();
  test_zero_timeout_wait_all_times_out_without_consuming();
  test_wait_all_completes_atomically_when_the_last_object_arrives();
  test_wait_any_reports_and_consumes_only_the_lowest_signalled_index();
  test_wait_any_wakes_on_a_later_signal();
  test_wait_all_times_out_on_schedule();
  test_wait_all_conserves_signals_under_contention();
  test_overlapping_wait_all_in_opposite_orders_does_not_deadlock();
  test_wait_all_deepens_a_mutant_the_waiter_already_owns();
  test_wait_any_wakes_when_a_thread_terminates();
  test_each_signal_wakes_wait_any_promptly();
  std::cout << "All multi-object wait semantics tests passed!\n";
  return 0;
}
