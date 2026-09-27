#pragma once

// Generic, reusable worker pool (Part 3 of the Recomp Analysis V2 pass).
// Xenon had no thread pool / parallel-for abstraction anywhere in the tree
// before this - see the audit in docs/recomp/RECOMP_ANALYSIS_V2.md. This is a small,
// dependency-free building block for distributing independent,
// index-addressable work across a fixed number of worker threads with a
// single deterministic merge point, used by the Recomp Driver to parallelize
// per-function static analysis (driver.cpp) and generated-code emission
// (generate_project()).
//
// This is deliberately NOT a general task-queue/thread-pool-with-futures
// design (no std::async-per-task, no one-thread-per-function, no detached
// workers - see Part 22's explicit "bad designs" list). The only operation
// is `parallel_for(count, fn)`: run `fn(i)` for every `i` in [0, count),
// blocking the caller until all of them complete. Determinism is the
// caller's responsibility to preserve by writing each index's result to
// storage owned by that index (e.g. a pre-sized `std::vector<Result>`
// indexed by `i`) rather than to shared mutable state - `fn` must never
// depend on, or care about, the order in which indices actually run.
//
// Implementation note: each parallel_for() call spawns worker_count() - 1
// std::thread workers (the calling thread participates as the remaining
// one), which claim indices via a shared atomic counter and are joined
// before the call returns - rather than keeping a pool of threads alive
// across calls and handing work off to them via shared dispatch state. An
// earlier version of this file tried the latter (a persistent pool with a
// "current job" pointer workers pick up); it repeatedly proved difficult to
// make airtight under real concurrent stress (a genuine, reproducible
// use-after-free was found and fixed, then another symptom resurfaced under
// heavier contention) and was not worth the risk for a component that is
// only ever invoked a modest number of times per analysis run (once per
// discovery wave or codegen batch, not once per function - see driver.cpp).
// std::thread::join() gives the exact happens-before guarantee this needs
// for free, is far easier to verify correct by inspection, and the thread
// creation cost (microseconds) is negligible next to a wave's actual work.

#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace xenon::recomp {

// Resolves a possibly-"auto" job count to an actual worker count.
// `requested`:
//   - nullopt  -> auto: max(1, hardware_concurrency - 1), leaving one logical
//                 CPU free for the OS/other work where more than one is
//                 available (Part 3's default policy).
//   - 1        -> exactly one worker (deterministic single-thread mode,
//                 kept available for debugging - Part 3).
//   - N (>= 1) -> exactly N workers; 0 is treated the same as "auto" so a
//                 caller that plumbs an unvalidated CLI integer through
//                 cannot accidentally request a zero-worker pool.
[[nodiscard]] std::size_t resolve_worker_count(std::optional<std::size_t> requested) noexcept;

// A fixed worker-count handle offering one operation: parallel_for(). See
// this header's top comment for the implementation strategy. Safe on
// Windows and Linux; uses only the standard library.
class WorkerPool {
 public:
  // `worker_count` is clamped to at least 1.
  explicit WorkerPool(std::size_t worker_count) : worker_count_(worker_count < 1 ? 1 : worker_count) {}

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;
  WorkerPool(WorkerPool&&) = delete;
  WorkerPool& operator=(WorkerPool&&) = delete;

  [[nodiscard]] std::size_t worker_count() const noexcept { return worker_count_; }

  // Runs fn(i) for every i in [0, count), distributing work across
  // worker_count() - 1 freshly spawned background threads plus the calling
  // thread (so worker_count() == 1 runs everything inline with no threading
  // overhead at all). Blocks until every index has been attempted and every
  // spawned thread has been joined.
  //
  // If one or more invocations of fn throw, the first exception observed
  // (in index order) is rethrown from this call after every already-claimed
  // index has finished running; indices not yet claimed at the time an
  // exception is first recorded are skipped (not silently lost - the caller
  // sees the exception and must treat the batch as incomplete). No
  // exception escapes a worker thread.
  void parallel_for(std::size_t count, const std::function<void(std::size_t)>& fn) const;

 private:
  std::size_t worker_count_;
};

}  // namespace xenon::recomp
