#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "xenon/gpu/backend_executor.hpp"

using xenon::gpu::BackendExecutor;
using namespace std::chrono_literals;

namespace {

// Every task runs, in the order handed over, whether it was posted, run, or
// executed inline because the worker was idle.
void test_tasks_run_in_order() {
  BackendExecutor executor;
  executor.start();
  std::vector<int> order;
  for (int i = 0; i < 2000; ++i) {
    if (i % 7 == 0)
      executor.run([&order, i] { order.push_back(i); });
    else
      executor.post([&order, i] { order.push_back(i); });
  }
  executor.wait_idle();
  assert(order.size() == 2000u);
  for (int i = 0; i < 2000; ++i) assert(order[i] == i);
  executor.stop();
}

// gpu::Backend is not thread-safe: no two tasks may overlap, including a
// run() executed inline on the caller while the worker holds back.
void test_tasks_never_overlap() {
  BackendExecutor executor;
  executor.start();
  std::atomic<int> active{0};
  std::atomic<int> overlaps{0};
  std::atomic<int> ran{0};
  const auto task = [&] {
    if (active.fetch_add(1) != 0) ++overlaps;
    std::this_thread::yield();
    ++ran;
    active.fetch_sub(1);
  };
  // A second producer exercises the inline/queued race from both sides.
  std::thread other([&] {
    for (int i = 0; i < 3000; ++i) {
      if (i % 3 == 0)
        executor.run(task);
      else
        executor.post(task);
    }
  });
  for (int i = 0; i < 3000; ++i) {
    if (i % 2 == 0)
      executor.run(task);
    else
      executor.post(task);
  }
  other.join();
  executor.wait_idle();
  assert(overlaps.load() == 0);
  assert(ran.load() == 6000);
  executor.stop();
}

// run() returns only after its task and everything before it have run, and
// runs on the caller when the worker has nothing to do.
void test_run_waits_and_runs_inline_when_idle() {
  BackendExecutor executor;
  executor.start();
  std::atomic<bool> slow_done{false};
  executor.post([&] {
    std::this_thread::sleep_for(20ms);
    slow_done = true;
  });
  std::thread::id ran_on{};
  bool saw_slow_done = false;
  executor.run([&] {
    saw_slow_done = slow_done.load();
    ran_on = std::this_thread::get_id();
  });
  assert(saw_slow_done);
  // Busy worker: the task was queued behind it and ran there.
  assert(ran_on != std::this_thread::get_id());
  assert(executor.statistics().waits >= 1u);

  executor.wait_idle();
  // Idle worker: run() executes here with no hand-off.
  const auto inline_before = executor.statistics().ran_inline;
  executor.run([&] { ran_on = std::this_thread::get_id(); });
  assert(ran_on == std::this_thread::get_id());
  assert(executor.statistics().ran_inline == inline_before + 1u);
  executor.stop();
}

// A task may hand over more work (a present task presenting through the
// graphics system): it runs at once, on the worker, without deadlocking.
void test_nested_tasks_run_inline_on_the_worker() {
  BackendExecutor executor;
  executor.start();
  std::vector<int> order;
  executor.post([&] {
    assert(executor.inside_task());
    order.push_back(1);
    executor.run([&] { order.push_back(2); });
    executor.post([&] { order.push_back(3); });
    executor.wait_idle();
    order.push_back(4);
  });
  executor.post([&] { order.push_back(5); });
  executor.wait_idle();
  assert((order == std::vector<int>{1, 2, 3, 4, 5}));
  executor.stop();
}

// Regression (AC6 window stayed blank): a present run inline on the pump
// presented through the graphics system, whose run() queued behind the
// inline task making it and waited for itself. A task run inline may hand
// over more work exactly as one on the worker may.
void test_nested_tasks_run_inline_on_the_caller() {
  BackendExecutor executor;
  executor.start();
  executor.wait_idle();
  std::vector<int> order;
  std::thread::id ran_on{};
  executor.run([&] {
    assert(executor.inside_task());
    order.push_back(1);
    executor.run([&] {
      order.push_back(2);
      ran_on = std::this_thread::get_id();
    });
    executor.post([&] { order.push_back(3); });
    executor.wait_idle();
    order.push_back(4);
  });
  assert(ran_on == std::this_thread::get_id());
  assert(!executor.inside_task());
  executor.post([&] { order.push_back(5); });
  executor.wait_idle();
  assert((order == std::vector<int>{1, 2, 3, 4, 5}));
  executor.stop();
}

// stop() runs everything still queued before joining; afterwards tasks run
// inline on the caller.
void test_stop_drains_the_queue() {
  BackendExecutor executor;
  executor.start();
  std::atomic<int> ran{0};
  for (int i = 0; i < 100; ++i)
    executor.post([&] {
      std::this_thread::sleep_for(50us);
      ++ran;
    });
  executor.stop();
  assert(ran.load() == 100);
  assert(!executor.running());
  std::thread::id ran_on{};
  executor.post([&] { ran_on = std::this_thread::get_id(); });
  assert(ran_on == std::this_thread::get_id());
  // Restartable.
  executor.start();
  executor.run([&] { ran_on = std::this_thread::get_id(); });
  executor.post([&] { ran_on = std::this_thread::get_id(); });
  executor.wait_idle();
  executor.stop();
}

// A task that throws is contained: the executor keeps going, the next tasks
// still run in order, and the failure is reported once.
void test_failures_are_contained_and_reported() {
  BackendExecutor executor;
  executor.start();
  std::vector<int> order;
  executor.post([&] { order.push_back(1); });
  executor.post([] { throw std::runtime_error("bad IR"); });
  executor.post([&] { order.push_back(2); });
  executor.run([&] { order.push_back(3); });
  assert((order == std::vector<int>{1, 2, 3}));
  assert(executor.take_error() == "bad IR");
  assert(executor.take_error().empty());
  assert(executor.statistics().task_failures == 1u);
  executor.stop();
}

// Never started: every task runs inline (tools, captures, tests).
void test_not_started_runs_inline() {
  BackendExecutor executor;
  std::thread::id ran_on{};
  executor.post([&] { ran_on = std::this_thread::get_id(); });
  assert(ran_on == std::this_thread::get_id());
  executor.run([&] { ran_on = {}; });
  assert(ran_on == std::thread::id{});
  executor.wait_idle();
  assert(executor.idle());
  // Inline mode propagates failures to the caller, as direct calls did.
  bool threw = false;
  try {
    executor.post([] { throw std::runtime_error("inline"); });
  } catch (const std::runtime_error&) {
    threw = true;
  }
  assert(threw);
}

}  // namespace

int main() {
  test_tasks_run_in_order();
  test_tasks_never_overlap();
  test_run_waits_and_runs_inline_when_idle();
  test_nested_tasks_run_inline_on_the_worker();
  test_nested_tasks_run_inline_on_the_caller();
  test_stop_drains_the_queue();
  test_failures_are_contained_and_reported();
  test_not_started_runs_inline();
  std::cout << "xenon_backend_executor_tests: ok\n";
  return 0;
}
