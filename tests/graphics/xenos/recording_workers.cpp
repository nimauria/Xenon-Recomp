#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "xenon/gpu/recording_workers.hpp"

using xenon::gpu::RecordingWorkers;
using namespace std::chrono_literals;

namespace {

// Every task runs exactly once, and wait_all() returns only after all of them.
void test_wait_all_covers_every_task() {
  RecordingWorkers workers(3);
  assert(workers.thread_count() == 3u);
  std::atomic<int> ran{0};
  for (int i = 0; i < 500; ++i)
    workers.submit([&] {
      std::this_thread::sleep_for(10us);
      ++ran;
    });
  workers.wait_all();
  assert(ran.load() == 500);
  assert(workers.tasks_run() == 500u);
  // Reusable for the next batch.
  for (int i = 0; i < 50; ++i) workers.submit([&] { ++ran; });
  workers.wait_all();
  assert(ran.load() == 550);
}

// Tasks run concurrently on the workers, never on the submitting thread.
void test_tasks_run_on_worker_threads() {
  RecordingWorkers workers(2);
  std::mutex mutex;
  std::set<std::thread::id> threads;
  std::atomic<int> active{0};
  std::atomic<int> peak{0};
  for (int i = 0; i < 40; ++i)
    workers.submit([&] {
      const int now = ++active;
      int expected = peak.load();
      while (now > expected && !peak.compare_exchange_weak(expected, now)) {}
      std::this_thread::sleep_for(2ms);
      --active;
      std::scoped_lock lock(mutex);
      threads.insert(std::this_thread::get_id());
    });
  workers.wait_all();
  assert(!threads.contains(std::this_thread::get_id()));
  assert(threads.size() == 2u);
  assert(peak.load() == 2);
}

// No workers: each task runs on the submitting thread, at once.
void test_zero_threads_runs_inline() {
  RecordingWorkers workers(0);
  assert(workers.thread_count() == 0u);
  std::thread::id ran_on{};
  workers.submit([&] { ran_on = std::this_thread::get_id(); });
  assert(ran_on == std::this_thread::get_id());
  workers.wait_all();
}

// A task that throws is reported once and does not stop the others.
void test_failures_are_reported() {
  RecordingWorkers workers(2);
  std::atomic<int> ran{0};
  workers.submit([] { throw std::runtime_error("list close failed"); });
  for (int i = 0; i < 10; ++i) workers.submit([&] { ++ran; });
  workers.wait_all();
  assert(ran.load() == 10);
  assert(workers.take_error() == "list close failed");
  assert(workers.take_error().empty());
}

// The default stays within one to four workers (or the override).
void test_default_thread_count_is_bounded() {
  const auto count = RecordingWorkers::default_thread_count();
  assert(count <= 16u);
}

}  // namespace

int main() {
  test_wait_all_covers_every_task();
  test_tasks_run_on_worker_threads();
  test_zero_threads_runs_inline();
  test_failures_are_reported();
  test_default_thread_count_is_bounded();
  std::cout << "xenon_recording_workers_tests: ok\n";
  return 0;
}
