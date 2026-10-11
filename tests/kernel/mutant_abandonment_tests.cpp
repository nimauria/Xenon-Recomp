// Mutant abandonment (NT dispatcher semantics, which the Xbox 360 kernel
// derives from):
// - A thread that exits while owning a mutant abandons it: the mutant becomes
//   free, and the next wait to take it returns STATUS_ABANDONED
//   (WaitResult::Abandoned) instead of STATUS_SUCCESS, taking ownership.
// - The abandoned flag is cleared by that acquisition; later waits succeed
//   normally.
// - A WaitAny that takes an abandoned mutant reports its index; a WaitAll
//   that takes one reports index 0 and still takes every object.
// - The former owner can no longer release it.
// - A thread's exit abandons only what that thread still owns, and does so
//   before the thread itself is signalled.

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
#include "xenon/kernel/thread.hpp"
#include "xenon/kernel/wait.hpp"

using namespace xenon::kernel;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kThreadA = 7;
constexpr std::uint32_t kThreadB = 8;

std::shared_ptr<KernelThread> start_thread(std::function<std::uint32_t()> entry) {
  ThreadCreationParams params{};
  auto thread = std::make_shared<KernelThread>(std::move(entry), params);
  assert(thread->start());
  return thread;
}

void test_wait_on_an_abandoned_mutant_reports_abandoned_once() {
  auto mutant = std::make_shared<KernelMutant>(true, kThreadB);
  mutant->abandon();
  assert(!mutant->is_owned());
  assert(wait_for_single_object(mutant, 0ms, kThreadA) == WaitResult::Abandoned);
  assert(mutant->owner_thread_id() == kThreadA && mutant->recursion_count() == 1);
  assert(mutant->release(kThreadA));
  assert(wait_for_single_object(mutant, 0ms, kThreadA) == WaitResult::Success &&
         "the acquisition that reported abandonment clears it");
}

void test_former_owner_cannot_release_an_abandoned_mutant() {
  auto mutant = std::make_shared<KernelMutant>(true, kThreadB);
  mutant->abandon();
  assert(!mutant->release(kThreadB));
}

void test_wait_any_reports_the_abandoned_mutants_index() {
  auto never = std::make_shared<KernelEvent>(true, false);
  auto mutant = std::make_shared<KernelMutant>(true, kThreadB);
  mutant->abandon();
  std::uint32_t index = 99;
  const std::vector<std::shared_ptr<KernelObject>> objects{never, mutant};
  assert(wait_for_multiple_objects(objects, false, 0ms, &index, kThreadA) ==
         WaitResult::Abandoned);
  assert(index == 1 && mutant->owner_thread_id() == kThreadA);
}

void test_wait_all_with_an_abandoned_mutant_takes_everything_and_reports_index_0() {
  auto event = std::make_shared<KernelEvent>(false, true);  // auto-reset, signalled
  auto mutant = std::make_shared<KernelMutant>(true, kThreadB);
  mutant->abandon();
  std::uint32_t index = 99;
  const std::vector<std::shared_ptr<KernelObject>> objects{event, mutant};
  assert(wait_for_multiple_objects(objects, true, 0ms, &index, kThreadA) ==
         WaitResult::Abandoned);
  assert(index == 0);
  assert(!event->signaled() && "the WaitAll still consumed the event");
  assert(mutant->owner_thread_id() == kThreadA);
}

void test_exiting_thread_abandons_only_what_it_still_owns() {
  auto held = std::make_shared<KernelMutant>();
  auto held_recursively = std::make_shared<KernelMutant>();
  auto released = std::make_shared<KernelMutant>();
  auto someone_elses = std::make_shared<KernelMutant>(true, kThreadA);
  auto worker = start_thread([&]() -> std::uint32_t {
    assert(wait_for_single_object(held, 0ms, kThreadB) == WaitResult::Success);
    assert(wait_for_single_object(held_recursively, 0ms, kThreadB) == WaitResult::Success);
    assert(wait_for_single_object(held_recursively, 0ms, kThreadB) == WaitResult::Success);
    assert(wait_for_single_object(released, 0ms, kThreadB) == WaitResult::Success);
    assert(released->release(kThreadB));
    return 0;
  });
  assert(worker->join());
  assert(!held->is_owned() && !held_recursively->is_owned());
  assert(wait_for_single_object(held, 0ms, kThreadA) == WaitResult::Abandoned);
  assert(wait_for_single_object(held_recursively, 0ms, kThreadA) == WaitResult::Abandoned);
  assert(wait_for_single_object(released, 0ms, kThreadA) == WaitResult::Success &&
         "a mutant released before exit is not abandoned");
  assert(someone_elses->owner_thread_id() == kThreadA &&
         "another thread's mutant is untouched by this thread's exit");
}

void test_blocked_waiter_wakes_with_abandoned_when_the_owner_exits() {
  auto mutant = std::make_shared<KernelMutant>();
  std::atomic<bool> owned{false};
  std::atomic<bool> exit_now{false};
  auto owner = start_thread([&]() -> std::uint32_t {
    assert(wait_for_single_object(mutant, 0ms, kThreadB) == WaitResult::Success);
    owned = true;
    while (!exit_now.load()) std::this_thread::sleep_for(1ms);
    return 0;
  });
  while (!owned.load()) std::this_thread::sleep_for(1ms);
  std::atomic<WaitResult> result{WaitResult::Failed};
  std::thread waiter([&] { result = wait_for_single_object(mutant, 5000ms, kThreadA); });
  std::this_thread::sleep_for(30ms);
  assert(result.load() == WaitResult::Failed && "the waiter blocks while the owner runs");
  const auto exited = std::chrono::steady_clock::now();
  exit_now = true;
  waiter.join();
  assert(result.load() == WaitResult::Abandoned);
  assert(std::chrono::steady_clock::now() - exited < 2000ms);
  assert(mutant->owner_thread_id() == kThreadA);
  assert(owner->join());
}

// A multi-object waiter sleeps on its own wait block, not on the mutant's
// condition variable, so abandonment must wake the mutant's wait list too.
void test_wait_any_wakes_with_abandoned_when_the_owner_exits() {
  auto never = std::make_shared<KernelEvent>(true, false);
  auto mutant = std::make_shared<KernelMutant>();
  std::atomic<bool> owned{false};
  std::atomic<bool> exit_now{false};
  auto owner = start_thread([&]() -> std::uint32_t {
    assert(wait_for_single_object(mutant, 0ms, kThreadB) == WaitResult::Success);
    owned = true;
    while (!exit_now.load()) std::this_thread::sleep_for(1ms);
    return 0;
  });
  while (!owned.load()) std::this_thread::sleep_for(1ms);
  std::uint32_t index = 99;
  std::atomic<WaitResult> result{WaitResult::Failed};
  std::thread waiter([&] {
    const std::vector<std::shared_ptr<KernelObject>> objects{never, mutant};
    result = wait_for_multiple_objects(objects, false, 5000ms, &index, kThreadA);
  });
  std::this_thread::sleep_for(30ms);
  const auto exited = std::chrono::steady_clock::now();
  exit_now = true;
  waiter.join();
  assert(result.load() == WaitResult::Abandoned && index == 1);
  assert(std::chrono::steady_clock::now() - exited < 2000ms);
  assert(owner->join());
}

void test_mutants_are_abandoned_before_the_thread_is_signalled() {
  auto mutant = std::make_shared<KernelMutant>();
  std::atomic<bool> exit_now{false};
  auto owner = start_thread([&]() -> std::uint32_t {
    assert(wait_for_single_object(mutant, 0ms, kThreadB) == WaitResult::Success);
    while (!exit_now.load()) std::this_thread::sleep_for(1ms);
    return 0;
  });
  while (!mutant->is_owned()) std::this_thread::sleep_for(1ms);
  std::atomic<bool> owned_when_signalled{true};
  std::thread waiter([&] {
    const std::vector<std::shared_ptr<KernelObject>> objects{owner};
    std::uint32_t index = 99;
    assert(wait_for_multiple_objects(objects, false, 5000ms, &index, kThreadA) ==
           WaitResult::Success);
    owned_when_signalled = mutant->is_owned();
  });
  std::this_thread::sleep_for(30ms);
  exit_now = true;
  waiter.join();
  assert(!owned_when_signalled.load());
  assert(owner->join());
}

// Each thread in turn takes the mutant from the previous owner's exit and
// exits holding it: one fresh acquisition, then one abandoned acquisition per
// thread after it.
void test_chain_of_exiting_owners() {
  constexpr int kThreads = 16;
  auto mutant = std::make_shared<KernelMutant>();
  std::atomic<int> successes{0};
  std::atomic<int> abandonments{0};
  std::vector<std::shared_ptr<KernelThread>> threads;
  for (int i = 0; i < kThreads; ++i) {
    const auto id = static_cast<std::uint32_t>(100 + i);
    threads.push_back(start_thread([&, id]() -> std::uint32_t {
      const auto result = wait_for_single_object(mutant, 10000ms, id);
      if (result == WaitResult::Success) ++successes;
      if (result == WaitResult::Abandoned) ++abandonments;
      return 0;
    }));
  }
  for (auto& thread : threads) assert(thread->join(20000));
  assert(successes.load() == 1 && abandonments.load() == kThreads - 1);
  assert(!mutant->is_owned());
}

}  // namespace

int main() {
  std::cout << "Testing mutant abandonment...\n";
  test_wait_on_an_abandoned_mutant_reports_abandoned_once();
  test_former_owner_cannot_release_an_abandoned_mutant();
  test_wait_any_reports_the_abandoned_mutants_index();
  test_wait_all_with_an_abandoned_mutant_takes_everything_and_reports_index_0();
  test_exiting_thread_abandons_only_what_it_still_owns();
  test_blocked_waiter_wakes_with_abandoned_when_the_owner_exits();
  test_wait_any_wakes_with_abandoned_when_the_owner_exits();
  test_mutants_are_abandoned_before_the_thread_is_signalled();
  test_chain_of_exiting_owners();
  std::cout << "All mutant abandonment tests passed!\n";
  return 0;
}
