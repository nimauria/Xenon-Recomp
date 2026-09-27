// Generic worker pool correctness (Part 3/21 of the Recomp Analysis V2
// pass): independent of the Recomp Driver's own use of it.

#include <atomic>
#include <cassert>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "xenon/recomp/worker_pool.hpp"

using xenon::recomp::WorkerPool;
using xenon::recomp::resolve_worker_count;

int main() {
  // resolve_worker_count(): explicit values pass through, "auto" (nullopt)
  // never returns 0, and 0 is treated the same as "auto".
  assert(resolve_worker_count(1) == 1);
  assert(resolve_worker_count(7) == 7);
  assert(resolve_worker_count(std::nullopt) >= 1);
  assert(resolve_worker_count(0) == resolve_worker_count(std::nullopt));

  // Every index in [0, count) runs exactly once, and results land at the
  // index that produced them regardless of which worker/thread ran it -
  // this is the "worker-local output + deterministic merge by index" pattern
  // the Recomp Driver relies on.
  {
    WorkerPool pool(4);
    constexpr std::size_t kCount = 5000;
    std::vector<std::size_t> results(kCount, static_cast<std::size_t>(-1));
    pool.parallel_for(kCount, [&](std::size_t i) { results[i] = i * 2u; });
    for (std::size_t i = 0; i < kCount; ++i) assert(results[i] == i * 2u);
  }

  // A pool of 1 worker still correctly executes every index (degenerate
  // but valid "deterministic single-thread mode" configuration) with zero
  // background threads spawned.
  {
    WorkerPool pool(1);
    std::vector<int> results(100, 0);
    pool.parallel_for(100, [&](std::size_t i) { results[i] = static_cast<int>(i) + 1; });
    for (std::size_t i = 0; i < 100; ++i) assert(results[i] == static_cast<int>(i) + 1);
  }

  // Zero-count call is a safe no-op.
  {
    WorkerPool pool(4);
    bool called = false;
    pool.parallel_for(0, [&](std::size_t) { called = true; });
    assert(!called);
  }

  // Exception propagation: an exception thrown from any index's fn is
  // observed by the caller after parallel_for() returns, and a second,
  // independent call on the same pool afterward still works correctly (the
  // pool itself is not left in a broken state by a prior worker exception).
  {
    WorkerPool pool(4);
    constexpr std::size_t kCount = 200;
    std::atomic<std::size_t> completed{0};
    bool threw = false;
    try {
      pool.parallel_for(kCount, [&](std::size_t i) {
        completed.fetch_add(1, std::memory_order_relaxed);
        if (i == 37) throw std::runtime_error("synthetic failure");
      });
    } catch (const std::runtime_error& error) {
      threw = true;
      assert(std::string(error.what()) == "synthetic failure");
    }
    assert(threw && "an exception thrown from a worker must be observed by the caller");
    std::vector<int> results(50, -1);
    pool.parallel_for(50, [&](std::size_t i) { results[i] = static_cast<int>(i); });
    for (std::size_t i = 0; i < 50; ++i) assert(results[i] == static_cast<int>(i));
  }

  // Repeated stress: many back-to-back batches on one pool at the platform's
  // full auto-detected worker count, checking for races (a wrong/missing
  // result would indicate one) - run under ThreadSanitizer in CI where
  // available (Part 21.16).
  {
    WorkerPool pool(resolve_worker_count(std::nullopt));
    for (int round = 0; round < 200; ++round) {
      constexpr std::size_t kCount = 613;  // deliberately not a multiple of common worker counts
      std::vector<std::size_t> results(kCount, static_cast<std::size_t>(-1));
      pool.parallel_for(kCount, [&](std::size_t i) { results[i] = i; });
      for (std::size_t i = 0; i < kCount; ++i) assert(results[i] == i);
    }
  }

  return 0;
}
