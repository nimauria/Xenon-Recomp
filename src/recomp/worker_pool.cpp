#include "xenon/recomp/worker_pool.hpp"

#include <atomic>
#include <algorithm>

namespace xenon::recomp {

std::size_t resolve_worker_count(std::optional<std::size_t> requested) noexcept {
  if (requested.has_value() && *requested != 0) return *requested;
  const auto hardware = std::thread::hardware_concurrency();
  if (hardware <= 1) return 1;
  return static_cast<std::size_t>(hardware) - 1u;
}

void WorkerPool::parallel_for(std::size_t count, const std::function<void(std::size_t)>& fn) const {
  if (count == 0) return;

  std::atomic<std::size_t> next_index{0};
  std::atomic<bool> failed{false};
  std::mutex exception_mutex;
  std::exception_ptr exception;

  // Claims and runs indices until either the work is exhausted or a failure
  // has been recorded (by this thread or another) - at which point it stops
  // claiming new work and returns. No shared "how many are left" counter is
  // needed for correctness: every spawned thread is join()ed below, and
  // join() alone guarantees this function has fully returned (including,
  // transitively, every write it made) before the thread object is
  // destroyed - the exact happens-before this needs, provided directly by
  // the standard rather than hand-rolled bookkeeping.
  const auto claim_and_run = [&] {
    for (;;) {
      if (failed.load(std::memory_order_relaxed)) return;
      const auto index = next_index.fetch_add(1, std::memory_order_relaxed);
      if (index >= count) return;
      try {
        fn(index);
      } catch (...) {
        std::lock_guard<std::mutex> lock(exception_mutex);
        if (!exception) exception = std::current_exception();
        failed.store(true, std::memory_order_relaxed);
      }
    }
  };

  const auto worker_thread_count = std::min(worker_count_, count) - 1;
  std::vector<std::jthread> threads;
  threads.reserve(worker_thread_count);
  for (std::size_t i = 0; i < worker_thread_count; ++i) threads.emplace_back(claim_and_run);
  // The calling thread also participates: for a small batch this means the
  // work is very likely to finish entirely on the calling thread before any
  // background thread even gets scheduled, and it means worker_count() - 1
  // background threads deliver worker_count()-way parallelism overall.
  claim_and_run();
  for (auto& thread : threads) thread.join();

  if (exception) std::rethrow_exception(exception);
}

}  // namespace xenon::recomp
