// terminate() must end a kernel wait the terminated thread is blocked in.
//
// Without that, a guest thread parked in an unbounded wait can never be
// stopped: session shutdown either hangs joining it or detaches a host thread
// that still runs against session state. On hardware, terminating a waiting
// thread ends its wait; here the wait returns WaitResult::Terminating, consumes
// nothing, and the guest dispatch loop stops the thread at its next block
// boundary.

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
#include "xenon/kernel/wait.hpp"

using namespace xenon::kernel;
using namespace std::chrono_literals;

namespace {

constexpr auto kInfinite = std::chrono::milliseconds(-1);

// Starts a thread that runs `wait` once, records its result and returns.
// The test terminates it while it waits and requires the join to succeed.
void expect_terminate_ends_wait(const char* what, std::function<WaitResult()> wait) {
  std::atomic<bool> waiting{false};
  std::atomic<WaitResult> result{WaitResult::Success};
  ThreadCreationParams params{};
  auto thread = std::make_shared<KernelThread>(
      [&]() -> std::uint32_t {
        waiting = true;
        result = wait();
        return 0;
      },
      params);
  assert(thread->start());
  while (!waiting.load()) std::this_thread::sleep_for(1ms);
  std::this_thread::sleep_for(30ms);
  const auto terminated = std::chrono::steady_clock::now();
  assert(thread->terminate(3));
  const bool joined = thread->join(2000);
  if (!joined) std::cerr << "terminate() did not end the wait: " << what << '\n';
  assert(joined);
  assert(std::chrono::steady_clock::now() - terminated < 2000ms);
  assert(result.load() == WaitResult::Terminating);
  assert(thread->exit_code() == 3u);
}

void test_terminate_ends_single_object_waits() {
  auto event = std::make_shared<KernelEvent>(false, false);
  expect_terminate_ends_wait("event", [&] { return wait_for_single_object(event, kInfinite, 1); });
  auto semaphore = std::make_shared<KernelSemaphore>(0, 1);
  expect_terminate_ends_wait("semaphore",
                             [&] { return wait_for_single_object(semaphore, kInfinite, 1); });
  auto mutant = std::make_shared<KernelMutant>(true, 99);  // owned by another thread
  expect_terminate_ends_wait("mutant", [&] { return wait_for_single_object(mutant, kInfinite, 1); });
  assert(mutant->owner_thread_id() == 99u && "the terminated waiter took nothing");
}

void test_terminate_ends_multi_object_waits() {
  auto first = std::make_shared<KernelEvent>(false, false);
  auto second = std::make_shared<KernelSemaphore>(0, 1);
  const std::vector<std::shared_ptr<KernelObject>> objects{first, second};
  expect_terminate_ends_wait("WaitAny", [&] {
    std::uint32_t index = 99;
    return wait_for_multiple_objects(objects, false, kInfinite, &index, 1);
  });
  expect_terminate_ends_wait("WaitAll", [&] {
    return wait_for_multiple_objects(objects, true, kInfinite, nullptr, 1);
  });
}

void test_terminate_ends_a_delay() {
  expect_terminate_ends_wait("KeDelayExecutionThread",
                             [] { return delay_current_thread(std::chrono::minutes(10)); });
  assert(delay_current_thread(5ms) == WaitResult::Success && "not a KernelThread: plain sleep");
}

// A thread that has been terminated (its guest code still finishing its
// current block) does not get to take an object it starts waiting for.
void test_a_terminated_thread_takes_nothing() {
  auto event = std::make_shared<KernelEvent>(false, true);  // signalled
  std::atomic<bool> terminated{false};
  std::atomic<WaitResult> result{WaitResult::Success};
  ThreadCreationParams params{};
  auto thread = std::make_shared<KernelThread>(
      [&]() -> std::uint32_t {
        while (!terminated.load()) std::this_thread::sleep_for(1ms);
        result = wait_for_single_object(event, 0ms, 1);
        return 0;
      },
      params);
  assert(thread->start());
  std::this_thread::sleep_for(20ms);
  assert(thread->terminate(0));
  terminated = true;
  assert(thread->join(2000));
  assert(result.load() == WaitResult::Terminating);
  assert(event->signaled() && "the auto-reset event was not consumed");
}

// Waits on host threads that are not KernelThreads are unchanged, and a
// KernelThread that is not terminated still times out and succeeds normally.
void test_ordinary_waits_are_unchanged() {
  auto event = std::make_shared<KernelEvent>(false, false);
  assert(wait_for_single_object(event, 10ms, 1) == WaitResult::Timeout);
  std::atomic<WaitResult> timed_out{WaitResult::Success};
  std::atomic<WaitResult> signalled{WaitResult::Timeout};
  ThreadCreationParams params{};
  auto thread = std::make_shared<KernelThread>(
      [&]() -> std::uint32_t {
        timed_out = wait_for_single_object(event, 10ms, 1);
        signalled = wait_for_single_object(event, 5000ms, 1);
        return 0;
      },
      params);
  assert(thread->start());
  std::this_thread::sleep_for(50ms);
  event->set();
  assert(thread->join(5000));
  assert(timed_out.load() == WaitResult::Timeout);
  assert(signalled.load() == WaitResult::Success);
}

}  // namespace

int main() {
  std::cout << "Testing termination of blocked kernel waits...\n";
  test_terminate_ends_single_object_waits();
  test_terminate_ends_multi_object_waits();
  test_terminate_ends_a_delay();
  test_a_terminated_thread_takes_nothing();
  test_ordinary_waits_are_unchanged();
  std::cout << "All wait termination tests passed!\n";
  return 0;
}
