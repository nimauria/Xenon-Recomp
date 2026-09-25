// Phase 2 (AC6 Runtime Readiness pass): KernelThread lifecycle correctness.
//
// Regression coverage for three real bugs the audit found:
//   1. join(timeout_ms) always blocked unconditionally regardless of the
//      requested timeout (src/kernel/threading/thread.cpp's old TODO).
//   2. thread_main() unconditionally overwrote exit_code_/state_ with
//      entry_()'s natural return value even if terminate() had already set a
//      different, intentional exit code concurrently.
//   3. ThreadCreationParams had no way to request CREATE_SUSPENDED - a
//      thread always began running entry_() immediately.
//
// Plus a concurrent-joiners stress test for the new mutex-guarded join()
// path (multiple guest threads can legitimately wait on the same thread
// handle via NtWaitForSingleObjectEx).

#include "xenon/kernel/thread.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace xenon::kernel;
using namespace std::chrono_literals;

namespace {

void test_create_suspended_does_not_run_entry_until_resumed() {
  std::atomic<bool> entry_ran{false};
  ThreadCreationParams params{};
  params.create_suspended = true;

  KernelThread thread([&]() -> std::uint32_t {
    entry_ran.store(true);
    return 7;
  }, params);

  assert(thread.start());
  // The host thread exists and the object reports Suspended immediately,
  // but entry_() must not run yet no matter how long we wait.
  assert(thread.state() == ThreadState::Suspended);
  std::this_thread::sleep_for(100ms);
  assert(!entry_ran.load());
  assert(thread.state() == ThreadState::Suspended);

  assert(thread.resume());
  assert(thread.join(2000));
  assert(entry_ran.load());
  assert(thread.state() == ThreadState::Terminated);
  assert(thread.exit_code() == 7u);
}

void test_terminate_while_suspended_never_runs_entry() {
  std::atomic<bool> entry_ran{false};
  ThreadCreationParams params{};
  params.create_suspended = true;

  KernelThread thread([&]() -> std::uint32_t {
    entry_ran.store(true);
    return 1;
  }, params);

  assert(thread.start());
  assert(thread.terminate(0xABCDu));
  // A parked (never-resumed) thread must wake up and exit on terminate(),
  // not hang forever - join() with a generous but finite timeout proves
  // this rather than assuming it.
  assert(thread.join(2000));
  assert(!entry_ran.load());
  assert(thread.exit_code() == 0xABCDu);
}

void test_join_timeout_does_not_block_past_the_requested_duration() {
  std::atomic<bool> allowed_to_return{false};
  ThreadCreationParams params{};
  KernelThread thread([&]() -> std::uint32_t {
    while (!allowed_to_return.load()) {
      std::this_thread::sleep_for(1ms);
    }
    return 0;
  }, params);

  assert(thread.start());

  const auto start = std::chrono::steady_clock::now();
  const bool joined = thread.join(50);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  assert(!joined && "join() must report timeout, not silently succeed");
  // Generous upper bound: proves join() actually returned near the
  // requested 50ms instead of blocking for the thread's full lifetime.
  assert(elapsed < 2000ms);

  allowed_to_return.store(true);
  assert(thread.join(2000));
}

void test_terminate_during_natural_return_does_not_clobber_exit_code() {
  // Reproduces the exact historical race: terminate() sets a specific exit
  // code while entry_() is still running (not parked, not yet returned).
  // When entry_() later returns naturally, its return value must NOT
  // overwrite the exit code terminate() already committed.
  std::atomic<bool> entry_started{false};
  std::atomic<bool> allowed_to_return{false};
  ThreadCreationParams params{};
  KernelThread thread([&]() -> std::uint32_t {
    entry_started.store(true);
    while (!allowed_to_return.load()) {
      std::this_thread::sleep_for(1ms);
    }
    return 0x11111111u;  // Must never be observed as the final exit code.
  }, params);

  assert(thread.start());
  while (!entry_started.load()) {
    std::this_thread::sleep_for(1ms);
  }

  assert(thread.terminate(0x99999999u));
  // Let entry_() actually return naturally now that terminate() has already
  // committed its own exit code.
  allowed_to_return.store(true);
  assert(thread.join(2000));

  assert(thread.exit_code() == 0x99999999u &&
         "entry_()'s natural return value must not clobber terminate()'s exit code");
}

void test_concurrent_joiners_are_all_satisfied_safely() {
  ThreadCreationParams params{};
  KernelThread thread([]() -> std::uint32_t {
    std::this_thread::sleep_for(30ms);
    return 42;
  }, params);
  assert(thread.start());

  constexpr int kWaiterCount = 8;
  std::vector<std::thread> waiters;
  std::atomic<int> success_count{0};
  for (int i = 0; i < kWaiterCount; ++i) {
    waiters.emplace_back([&] {
      if (thread.join(5000)) {
        success_count.fetch_add(1);
      }
    });
  }
  for (auto& waiter : waiters) {
    waiter.join();
  }

  assert(success_count.load() == kWaiterCount);
  assert(thread.exit_code() == 42u);
}

}  // namespace

int main() {
  std::cout << "Testing KernelThread lifecycle...\n";

  test_create_suspended_does_not_run_entry_until_resumed();
  test_terminate_while_suspended_never_runs_entry();
  test_join_timeout_does_not_block_past_the_requested_duration();
  test_terminate_during_natural_return_does_not_clobber_exit_code();
  test_concurrent_joiners_are_all_satisfied_safely();

  std::cout << "All KernelThread lifecycle tests passed!\n";
  return 0;
}
