// Asynchronous pipeline creation (include/xenon/gpu/pipeline_build.hpp): the
// slot state machine, priority and promotion, failure handling, and the shader
// worker pool the backends run the builds on.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "xenon/gpu/perf_stats.hpp"
#include "xenon/gpu/pipeline_build.hpp"
#ifdef XENON_TEST_DXC
#include "xenon/gpu/shader_compile_queue.hpp"
#endif

namespace gpu = xenon::gpu;
namespace perf = gpu::perf;
using namespace std::chrono_literals;

namespace {

// What a backend's pipeline object looks like to PipelineBuilds.
struct FakePipeline {
  int built_value{};
  bool built{};
  void reset() noexcept {
    built_value = 0;
    built = false;
  }
};
using Builds = gpu::PipelineBuilds<FakePipeline>;

// A priority pool of `threads` workers, with the contract of the shader worker
// pool (urgent group first, promote) and nothing else.
class TestPool final : public gpu::PipelineExecutor {
 public:
  explicit TestPool(unsigned threads) {
    for (unsigned i = 0; i < threads; ++i) workers_.emplace_back([this] { work(); });
  }
  ~TestPool() override {
    {
      std::scoped_lock lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_all();
    for (auto& worker : workers_) worker.join();
  }
  void submit(std::uint64_t id, std::function<void()> task, bool urgent) override {
    {
      std::scoped_lock lock(mutex_);
      (urgent ? urgent_ : normal_).push_back({id, std::move(task)});
    }
    ready_.notify_one();
  }
  bool promote(std::uint64_t id) override {
    std::scoped_lock lock(mutex_);
    for (auto it = normal_.begin(); it != normal_.end(); ++it) {
      if (it->first != id) continue;
      urgent_.push_back(std::move(*it));
      normal_.erase(it);
      return true;
    }
    return false;
  }

 private:
  void work() {
    std::unique_lock lock(mutex_);
    for (;;) {
      ready_.wait(lock, [&] { return stopping_ || !urgent_.empty() || !normal_.empty(); });
      if (stopping_ && urgent_.empty() && normal_.empty()) return;
      auto& source = urgent_.empty() ? normal_ : urgent_;
      auto task = std::move(source.front().second);
      source.pop_front();
      lock.unlock();
      task();
      lock.lock();
    }
  }
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::pair<std::uint64_t, std::function<void()>>> urgent_, normal_;
  std::vector<std::thread> workers_;
  bool stopping_{};
};

// A gate a build blocks on until the test opens it.
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  bool open{};
  void wait() {
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return open; });
  }
  void release() {
    {
      std::scoped_lock lock(mutex);
      open = true;
    }
    cv.notify_all();
  }
};

Builds::Build ok_build(int value) {
  return [value](FakePipeline& pipeline, std::string&) {
    pipeline.built_value = value;
    pipeline.built = true;
    return true;
  };
}

void test_synchronous_without_an_executor() {
  Builds builds;
  assert(!builds.asynchronous());
  assert(builds.needs_build(1));
  auto& slot = builds.request(1, ok_build(7), true);
  // Built before request() returned: the XENON_ASYNC_PIPELINES=0 behaviour.
  assert(slot.state() == gpu::PipelineState::Ready);
  assert(slot.pipeline.built && slot.pipeline.built_value == 7);
  assert(!builds.needs_build(1));
  // A second request of a ready key is a no-op.
  auto& again = builds.request(1, ok_build(8), true);
  assert(&again == &slot && again.pipeline.built_value == 7);
  assert(builds.wait(slot) == gpu::PipelineState::Ready);
}

void test_kill_switch_reads_the_environment() {
#ifdef _WIN32
  _putenv_s("XENON_ASYNC_PIPELINES", "0");
  assert(!gpu::async_pipelines_enabled());
  _putenv_s("XENON_ASYNC_PIPELINES", "1");
  assert(gpu::async_pipelines_enabled());
  _putenv_s("XENON_ASYNC_PIPELINES", "");
#else
  setenv("XENON_ASYNC_PIPELINES", "0", 1);
  assert(!gpu::async_pipelines_enabled());
  setenv("XENON_ASYNC_PIPELINES", "1", 1);
  assert(gpu::async_pipelines_enabled());
  unsetenv("XENON_ASYNC_PIPELINES");
#endif
  assert(gpu::async_pipelines_enabled());
}

void test_a_pipeline_queued_at_prefetch_is_ready_by_the_draw() {
  TestPool pool(2);
  Builds builds;
  builds.set_executor(&pool);
  const auto queued_before = perf::lifetime(perf::Counter::PipelinesQueued);
  // The frontend queues it (not urgent) long before the draw is consumed.
  auto& slot = builds.request(42, ok_build(5), false);
  assert(slot.state() != gpu::PipelineState::Unknown);
  assert(perf::lifetime(perf::Counter::PipelinesQueued) == queued_before + 1u);
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (slot.state() != gpu::PipelineState::Ready && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  // The draw's own request finds it built: nothing to build, nothing to wait for.
  assert(!builds.needs_build(42));
  const auto waits_before = perf::lifetime(perf::Counter::PipelineWaits);
  const auto wait_ns_before = perf::lifetime(perf::Counter::PipelineWaitNs);
  assert(builds.wait(slot) == gpu::PipelineState::Ready);
  assert(slot.pipeline.built_value == 5);
  assert(perf::lifetime(perf::Counter::PipelineWaits) == waits_before);
  assert(perf::lifetime(perf::Counter::PipelineWaitNs) == wait_ns_before);
}

void test_waiting_for_a_slow_build_is_counted() {
  TestPool pool(1);
  Builds builds;
  builds.set_executor(&pool);
  Gate gate;
  auto& slot = builds.request(
      1,
      [&](FakePipeline& pipeline, std::string&) {
        gate.wait();
        pipeline.built = true;
        return true;
      },
      true);
  const auto waits_before = perf::lifetime(perf::Counter::PipelineWaits);
  std::thread opener([&] {
    std::this_thread::sleep_for(30ms);
    gate.release();
  });
  const auto start = std::chrono::steady_clock::now();
  assert(builds.wait(slot) == gpu::PipelineState::Ready);
  assert(std::chrono::steady_clock::now() - start >= 20ms);
  opener.join();
  assert(perf::lifetime(perf::Counter::PipelineWaits) == waits_before + 1u);
  assert(perf::lifetime(perf::Counter::PipelineWaitNs) >= 20'000'000u);
}

void test_failed_builds_are_reported_and_not_retried_forever() {
  TestPool pool(1);
  Builds builds;
  builds.set_executor(&pool);
  std::atomic<int> attempts{0};
  const auto failing = [&](FakePipeline&, std::string& error) {
    ++attempts;
    error = "driver rejected the pipeline";
    return false;
  };
  const auto failures_before = perf::lifetime(perf::Counter::PipelineFailures);
  auto& slot = builds.request(9, failing, true);
  assert(builds.wait(slot) == gpu::PipelineState::Failed);
  assert(slot.error() == "driver rejected the pipeline");
  assert(attempts == 1);
  assert(perf::lifetime(perf::Counter::PipelineFailures) == failures_before + 1u);
  // The next draws needing it build it again - a bounded number of times.
  for (unsigned i = 1; i < Builds::kMaxAttempts; ++i) {
    assert(builds.needs_build(9));
    auto& retry = builds.request(9, failing, true);
    assert(&retry == &slot);
    assert(builds.wait(retry) == gpu::PipelineState::Failed);
  }
  assert(attempts == int(Builds::kMaxAttempts));
  // Then the failure stands: no more builds, the error is still reported.
  assert(!builds.needs_build(9));
  auto& last = builds.request(9, failing, true);
  assert(builds.wait(last) == gpu::PipelineState::Failed);
  assert(last.error() == "driver rejected the pipeline");
  assert(attempts == int(Builds::kMaxAttempts));
  // A retry that succeeds makes the slot ready.
  auto& other = builds.request(10, [&](FakePipeline&, std::string&) { return attempts++ >= 4; }, true);
  assert(builds.wait(other) == gpu::PipelineState::Failed);
  assert(builds.needs_build(10));
  auto& fixed = builds.request(10, ok_build(3), true);
  assert(builds.wait(fixed) == gpu::PipelineState::Ready && fixed.pipeline.built_value == 3);
  // A build that throws is a failure, not a crash.
  auto& thrower = builds.request(
      11, [](FakePipeline&, std::string&) -> bool { throw std::runtime_error("boom"); }, true);
  assert(builds.wait(thrower) == gpu::PipelineState::Failed && thrower.error() == "boom");
}

void test_no_deadlock_with_one_worker() {
  // Many pipelines, one worker, a draw waiting for the last queued first: the
  // wait promotes it and every build still finishes.
  TestPool pool(1);
  Builds builds;
  builds.set_executor(&pool);
  std::vector<Builds::Slot*> slots;
  for (int i = 0; i < 40; ++i)
    slots.push_back(&builds.request(100 + i, ok_build(i), /*urgent=*/false));
  assert(builds.wait(*slots.back()) == gpu::PipelineState::Ready);
  for (std::size_t i = 0; i < slots.size(); ++i) {
    assert(builds.wait(*slots[i]) == gpu::PipelineState::Ready);
    assert(slots[i]->pipeline.built_value == int(i));
  }
  builds.drain();
  assert(builds.outstanding() == 0u);
}

void test_priority_ordering() {
  TestPool pool(1);
  Builds builds;
  builds.set_executor(&pool);
  Gate gate;
  std::mutex order_mutex;
  std::vector<int> order;
  const auto recording = [&](int id) -> Builds::Build {
    return [&, id](FakePipeline& pipeline, std::string&) {
      std::scoped_lock lock(order_mutex);
      order.push_back(id);
      pipeline.built = true;
      return true;
    };
  };
  // The only worker is busy; behind it: two prefetched builds, then a draw's.
  auto& blocker = builds.request(
      1,
      [&](FakePipeline& pipeline, std::string&) {
        gate.wait();
        pipeline.built = true;
        return true;
      },
      true);
  auto& prefetch_a = builds.request(2, recording(2), false);
  auto& prefetch_b = builds.request(3, recording(3), false);
  auto& prefetch_c = builds.request(4, recording(4), false);
  auto& draw_own = builds.request(5, recording(5), true);
  // A draw asking for a prefetched pipeline still queued moves it ahead of
  // the rest of the prefetched ones (and behind the urgent ones before it).
  (void)builds.request(4, recording(4), true);
  gate.release();
  for (auto* slot : {&blocker, &prefetch_a, &prefetch_b, &prefetch_c, &draw_own})
    assert(builds.wait(*slot) == gpu::PipelineState::Ready);
  assert((order == std::vector<int>{5, 4, 2, 3}));
}

void test_states_are_observable() {
  TestPool pool(1);
  Builds builds;
  builds.set_executor(&pool);
  Gate gate;
  std::promise<void> started;
  auto& first = builds.request(
      1,
      [&](FakePipeline&, std::string&) {
        started.set_value();
        gate.wait();
        return true;
      },
      true);
  started.get_future().wait();
  assert(first.state() == gpu::PipelineState::Compiling);
  auto& second = builds.request(2, ok_build(1), true);
  assert(second.state() == gpu::PipelineState::Queued);
  gate.release();
  assert(builds.wait(second) == gpu::PipelineState::Ready);
  assert(first.state() == gpu::PipelineState::Ready);
  assert(std::string(gpu::to_string(gpu::PipelineState::Failed)) == "failed");
}

#ifdef XENON_TEST_DXC
// The real worker pool: urgent tasks first, promotion, one worker.
void test_shader_worker_pool_runs_pipeline_tasks_in_priority_order() {
  auto compiler = std::make_shared<gpu::DxcShaderCompiler>();
  gpu::ShaderCache cache(compiler);
  gpu::ShaderCompileQueue queue(cache, 1);
  assert(queue.worker_count() == 1u);
  Gate gate;
  std::mutex mutex;
  std::vector<int> order;
  std::promise<void> running;
  queue.submit(1, [&] { running.set_value(); gate.wait(); }, true);
  running.get_future().wait();
  const auto record = [&](int id) {
    return [&, id] {
      std::scoped_lock lock(mutex);
      order.push_back(id);
    };
  };
  queue.submit(10, record(10), false);
  queue.submit(11, record(11), false);
  queue.submit(12, record(12), true);
  queue.submit(13, record(13), false);
  assert(queue.promote(13));
  assert(!queue.promote(13));   // already urgent
  assert(!queue.promote(999));  // unknown
  gate.release();
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (queue.pending() != 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  assert((order == std::vector<int>{12, 13, 10, 11}));

  // PipelineBuilds on it: no deadlock with the single worker.
  Builds builds;
  builds.set_executor(&queue);
  std::vector<Builds::Slot*> slots;
  for (int i = 0; i < 20; ++i) slots.push_back(&builds.request(200 + i, ok_build(i), i % 2 == 0));
  for (auto* slot : slots) assert(builds.wait(*slot) == gpu::PipelineState::Ready);
  builds.drain();
}
#endif

}  // namespace

int main() {
  test_synchronous_without_an_executor();
  test_kill_switch_reads_the_environment();
  test_a_pipeline_queued_at_prefetch_is_ready_by_the_draw();
  test_waiting_for_a_slow_build_is_counted();
  test_failed_builds_are_reported_and_not_retried_forever();
  test_no_deadlock_with_one_worker();
  test_priority_ordering();
  test_states_are_observable();
#ifdef XENON_TEST_DXC
  test_shader_worker_pool_runs_pipeline_tasks_in_priority_order();
#endif
  std::cout << "pipeline build tests passed\n";
  return 0;
}
