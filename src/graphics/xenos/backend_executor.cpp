#include "xenon/gpu/backend_executor.hpp"

#include <chrono>
#include <exception>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define XENON_SPIN_PAUSE() _mm_pause()
#else
#define XENON_SPIN_PAUSE() std::this_thread::yield()
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "xenon/gpu/perf_stats.hpp"
#include "xenon/logging/activity_trace.hpp"

namespace xenon::gpu {
namespace {

using Clock = std::chrono::steady_clock;

// A drain hands work over in bursts: a few draws, a barrier, a few more.
// Spinning this long before sleeping keeps the hand-off off the scheduler's
// wake-up latency (tens of microseconds on Windows), which at AC6's ~2000
// barriers a second would cost more than the overlap gains. Bounded, so an
// idle executor sleeps.
constexpr auto kSpin = std::chrono::microseconds(50);

// The executor whose task this thread is executing, if any.
thread_local const BackendExecutor* t_executing = nullptr;

std::uint64_t ns_since(Clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

void name_worker_thread() {
#if defined(_WIN32)
  // Shown by debuggers and profilers; failure is harmless.
  (void)SetThreadDescription(GetCurrentThread(), L"Xenon GPU backend");
#endif
  logging::set_activity_thread_label("gpu backend");
}

}  // namespace

void BackendExecutor::start() {
  std::scoped_lock lock(mutex_);
  if (thread_.joinable()) return;
  stopping_ = false;
  thread_ = std::thread([this] { run_worker(); });
  worker_id_.store(thread_.get_id(), std::memory_order_release);
  running_.store(true, std::memory_order_release);
}

void BackendExecutor::stop() noexcept {
  {
    std::scoped_lock lock(mutex_);
    if (!thread_.joinable()) return;
    stopping_ = true;
  }
  work_.notify_all();
  thread_.join();
  std::scoped_lock lock(mutex_);
  // Only now: until the worker has drained the queue, a run() on another
  // thread must not execute inline beside it.
  running_.store(false, std::memory_order_release);
  worker_id_.store(std::thread::id{}, std::memory_order_release);
  thread_ = std::thread{};
  stopping_ = false;
}

bool BackendExecutor::inside_task() const noexcept {
  return running() &&
         (t_executing == this ||
          std::this_thread::get_id() == worker_id_.load(std::memory_order_acquire));
}

void BackendExecutor::execute(const Task& task, bool on_worker) {
  const auto start = Clock::now();
  const auto* const outer = t_executing;
  t_executing = this;
  struct Restore {
    const BackendExecutor* outer;
    ~Restore() { t_executing = outer; }
  } restore{outer};
  try {
    task();
  } catch (const std::exception& ex) {
    std::scoped_lock lock(mutex_);
    error_ = ex.what();
    ++stats_.task_failures;
  } catch (...) {
    std::scoped_lock lock(mutex_);
    error_ = "backend task threw a non-standard exception";
    ++stats_.task_failures;
  }
  const auto ns = ns_since(start);
  if (on_worker) perf::add(perf::Counter::BackendNs, ns);
}

void BackendExecutor::run_worker() {
  name_worker_thread();
  std::unique_lock lock(mutex_);
  for (;;) {
    if (queue_.empty() || inline_running_) {
      if (stopping_ && queue_.empty() && !inline_running_) break;
      lock.unlock();
      const auto spin_start = Clock::now();
      while (pending_.load(std::memory_order_acquire) == 0u && Clock::now() - spin_start < kSpin)
        XENON_SPIN_PAUSE();
      lock.lock();
      work_.wait(lock, [this] {
        return (!queue_.empty() && !inline_running_) ||
               (stopping_ && queue_.empty() && !inline_running_);
      });
      continue;
    }
    Item item = std::move(queue_.front());
    queue_.pop_front();
    worker_running_task_ = true;
    lock.unlock();
    const auto start = Clock::now();
    execute(item.task, true);
    const auto busy = ns_since(start);
    item.task = nullptr;  // release captured IR outside the lock's critical path
    lock.lock();
    worker_running_task_ = false;
    stats_.worker_busy_ns += busy;
    ++stats_.ran_on_worker;
    completed_sequence_.store(item.sequence, std::memory_order_release);
    pending_.fetch_sub(1u, std::memory_order_acq_rel);
    done_.notify_all();
  }
}

std::uint64_t BackendExecutor::post_sequenced(Task task) {
  if (!running() || inside_task()) {
    task();
    return 0u;
  }
  std::uint64_t sequence = 0;
  {
    std::scoped_lock lock(mutex_);
    sequence = ++next_sequence_;
    queue_.push_back(Item{std::move(task), sequence});
    pending_.fetch_add(1u, std::memory_order_acq_rel);
    ++stats_.posted;
    // perf_frame.log "queue": how far the backend runs behind.
    perf::note_max(perf::Counter::BackendQueueMax, queue_.size());
    perf::add(perf::Counter::BackendQueueDepthSum, queue_.size());
    perf::add(perf::Counter::BackendPosts, 1u);
  }
  work_.notify_one();
  return sequence;
}

void BackendExecutor::run(const Task& task) {
  if (!running() || inside_task()) {
    task();
    return;
  }
  std::uint64_t sequence = 0;
  {
    std::unique_lock lock(mutex_);
    if (queue_.empty() && !worker_running_task_ && !inline_running_) {
      // Everything handed over before has finished and the worker is held
      // off while inline_running_ is set: run here, with no hand-off.
      sequence = ++next_sequence_;
      inline_running_ = true;
      ++stats_.ran_inline;
      lock.unlock();
      execute(task, false);
      lock.lock();
      inline_running_ = false;
      completed_sequence_.store(sequence, std::memory_order_release);
      lock.unlock();
      work_.notify_one();
      done_.notify_all();
      return;
    }
    sequence = ++next_sequence_;
    queue_.push_back(Item{task, sequence});
    pending_.fetch_add(1u, std::memory_order_acq_rel);
    ++stats_.posted;
  }
  work_.notify_one();
  wait_for(sequence);
}

void BackendExecutor::wait_idle() {
  if (!running() || inside_task()) return;
  std::uint64_t sequence = 0;
  {
    std::scoped_lock lock(mutex_);
    sequence = next_sequence_;
  }
  wait_for(sequence);
}

void BackendExecutor::wait_for(std::uint64_t sequence) {
  if (completed_sequence_.load(std::memory_order_acquire) >= sequence) return;
  const auto start = Clock::now();
  while (completed_sequence_.load(std::memory_order_acquire) < sequence &&
         Clock::now() - start < kSpin)
    XENON_SPIN_PAUSE();
  if (completed_sequence_.load(std::memory_order_acquire) < sequence) {
    std::unique_lock lock(mutex_);
    done_.wait(lock, [&] {
      return completed_sequence_.load(std::memory_order_acquire) >= sequence;
    });
  }
  const auto waited = ns_since(start);
  perf::add(perf::Counter::BackendWaitNs, waited);
  perf::add(perf::Counter::BackendWaits, 1u);
  std::scoped_lock lock(mutex_);
  ++stats_.waits;
  stats_.wait_ns += waited;
}

BackendExecutor::Statistics BackendExecutor::statistics() const {
  std::scoped_lock lock(mutex_);
  return stats_;
}

std::string BackendExecutor::take_error() {
  std::scoped_lock lock(mutex_);
  std::string error;
  error.swap(error_);
  return error;
}

}  // namespace xenon::gpu
