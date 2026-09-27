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

// Real bug found while auditing the KeTlsAlloc/KeTlsGetValue/KeTlsSetValue
// exports: ThreadManager::get_thread() returns an OWNING std::shared_ptr, so
// a caller running as guest code on the thread it is asking about (the
// common case for these exports, which resolve ExportCallContext::thread_id)
// could accidentally drop the map's last *other* reference via a temporary
// going out of scope, destroying the KernelThread object while it is still
// executing on its own call stack - intermittent heap corruption, not a
// clean crash. get_thread_ptr() returns a non-owning raw pointer instead
// (the verified-safe pattern rexglue-sdk's XThread::GetCurrentThread() also
// uses), which can never trigger destruction.
void test_get_thread_ptr_is_non_owning() {
  ThreadManager manager;
  ThreadCreationParams params{};
  auto thread = manager.create_thread([]() -> std::uint32_t { return 0; }, params);
  const auto id = thread->thread_id();

  auto* ptr = manager.get_thread_ptr(id);
  assert(ptr == thread.get());
  assert(manager.get_thread_ptr(id + 1000u) == nullptr);

  // Dropping every OTHER reference must not affect a raw pointer previously
  // obtained via get_thread_ptr() - proving it is genuinely non-owning.
  thread.reset();
  assert(manager.get_thread_ptr(id) != nullptr);
}

// Regression test for ThreadManager::reap_finished_threads() - the
// mechanism that replaces the old, hazardous pattern of a thread removing
// itself from the map right before its own entry_() returns (see
// XenonSession::run_execution()/run_created_guest_thread()). Proves it
// removes only threads that have actually finished, leaving a still-running
// thread untouched.
void test_reap_finished_threads_removes_only_terminated_threads() {
  ThreadManager manager;
  ThreadCreationParams params{};

  auto finished = manager.create_thread([]() -> std::uint32_t { return 0; }, params);
  const auto finished_id = finished->thread_id();
  assert(finished->start());
  assert(finished->join(2000));

  // Drop the test's own reference to `finished` - the manager's map entry is
  // the only remaining reference, so reap can actually erase-and-destroy it.
  // Reap explicitly here, BEFORE creating the still-running thread below:
  // create_thread() itself opportunistically reaps (see ThreadManager's
  // implementation), so creating `running` first would already remove
  // `finished` as a side effect and make this call's return value 0, not 1 -
  // this ordering isolates reap_finished_threads()'s own return value from
  // that opportunistic side channel.
  finished.reset();
  assert(manager.reap_finished_threads() == 1u);
  assert(manager.get_thread_ptr(finished_id) == nullptr);

  std::atomic<bool> allowed_to_return{false};
  auto running = manager.create_thread([&]() -> std::uint32_t {
    while (!allowed_to_return.load()) {
      std::this_thread::sleep_for(1ms);
    }
    return 0;
  }, params);
  const auto running_id = running->thread_id();
  assert(running->start());

  // A still-running thread must survive a reap untouched.
  assert(manager.reap_finished_threads() == 0u);
  assert(manager.get_thread_ptr(running_id) != nullptr);

  allowed_to_return.store(true);
  assert(running->join(2000));
  running.reset();
  assert(manager.reap_finished_threads() == 1u);
  assert(manager.get_thread_ptr(running_id) == nullptr);
}

// The critical self-safety proof for ThreadManager::reap_finished_threads():
// it must be safe to call from a thread's OWN entry_() even when that
// thread's is_terminated() is already true - which can happen mid-dispatch
// via an externally-issued terminate() (KernelThread::terminate() sets
// terminated_ immediately regardless of which thread calls it), not only at
// natural completion. This is the exact shape of the call sites in
// XenonSession::run_execution()/run_created_guest_thread(): both call
// reap_finished_threads() from inside their own entry_(), after
// dispatch_guest_thread() has already observed is_terminated()==true for
// the very thread that's still running. A naive implementation keyed only
// on is_terminated() (rather than KernelThread::is_current_host_thread())
// would destroy the calling thread's own KernelThread object right here -
// the same use-after-free class this whole mechanism exists to prevent.
void test_self_reap_never_destroys_the_calling_thread() {
  ThreadManager manager;
  ThreadCreationParams params{};

  std::atomic<bool> entry_started{false};
  std::atomic<bool> reap_returned_without_crashing{false};
  std::atomic<bool> continuation_completed{false};
  // Chicken-and-egg: the entry closure needs to read the very shared_ptr
  // create_thread() is about to return, so it's captured by reference and
  // assigned before start() is called - thread_main() cannot observe it
  // unset because the host OS thread doesn't exist until start() runs.
  std::shared_ptr<KernelThread> self_thread;

  self_thread = manager.create_thread([&]() -> std::uint32_t {
    entry_started.store(true);
    // Wait for the test's thread to terminate() us from the outside while
    // we are still running here - simulating an externally-issued,
    // mid-dispatch preemptive terminate.
    while (!self_thread->is_terminated()) {
      std::this_thread::sleep_for(1ms);
    }
    // The hazardous call shape under test: reap while is_terminated() is
    // already true for the very thread making this call.
    manager.reap_finished_threads();
    reap_returned_without_crashing.store(true);
    // Keep running a bit longer - if reap had incorrectly destroyed this
    // KernelThread out from under us, touching shared state afterward would
    // corrupt or crash rather than reach here cleanly.
    std::this_thread::sleep_for(20ms);
    continuation_completed.store(true);
    return 0x11111111u;  // Must never be observed as the final exit code.
  }, params);

  const auto id = self_thread->thread_id();
  assert(self_thread->start());
  while (!entry_started.load()) {
    std::this_thread::sleep_for(1ms);
  }

  assert(self_thread->terminate(0x99999999u));
  assert(self_thread->join(2000));
  assert(reap_returned_without_crashing.load());
  assert(continuation_completed.load());
  assert(self_thread->exit_code() == 0x99999999u &&
         "terminate()'s exit code must survive the thread's own reap call and "
         "its subsequent natural return");

  // The thread's own reap call must not have removed itself from the map.
  assert(manager.get_thread_ptr(id) != nullptr);

  self_thread.reset();
  assert(manager.reap_finished_threads() == 1u);
  assert(manager.get_thread_ptr(id) == nullptr);
}

}  // namespace

int main() {
  std::cout << "Testing KernelThread lifecycle...\n";

  test_create_suspended_does_not_run_entry_until_resumed();
  test_terminate_while_suspended_never_runs_entry();
  test_join_timeout_does_not_block_past_the_requested_duration();
  test_terminate_during_natural_return_does_not_clobber_exit_code();
  test_concurrent_joiners_are_all_satisfied_safely();
  test_get_thread_ptr_is_non_owning();
  test_reap_finished_threads_removes_only_terminated_threads();
  test_self_reap_never_destroys_the_calling_thread();

  std::cout << "All KernelThread lifecycle tests passed!\n";
  return 0;
}
