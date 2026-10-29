#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "xenon/kernel/time.hpp"

using namespace std::chrono_literals;
using xenon::kernel::HostTimerResolution;

namespace {
// The median of `count` host sleeps of 1 ms, as KeDelayExecutionThread and
// the GPU worker's wait polls issue them.
std::chrono::microseconds median_one_ms_sleep(int count) {
  std::vector<std::chrono::microseconds> samples;
  for (int i = 0; i < count; ++i) {
    const auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(1ms);
    samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start));
  }
  std::nth_element(samples.begin(), samples.begin() + count / 2, samples.end());
  return samples[count / 2];
}

// AC6 ran at ~4 frames a second: each 1 ms guest sleep and GPU park poll took
// a 15.6 ms Windows tick. While the session's scope is held, the host runs
// its timed waits at a millisecond or finer, and a 1 ms sleep lasts about that.
void test_scope_raises_resolution() {
  const HostTimerResolution scope{};
  assert(scope.active());
  assert(HostTimerResolution::current() <= 1ms);
  const auto median = median_one_ms_sleep(31);
  std::cout << "median 1 ms sleep: " << median.count() << " us\n";
  assert(median < 4ms);
}

// Scopes nest; releasing an inner one leaves the outer request in force.
void test_scopes_nest() {
  const HostTimerResolution outer{};
  {
    const HostTimerResolution inner{};
    assert(inner.active());
  }
  assert(outer.active());
  assert(HostTimerResolution::current() <= 1ms);
}
}  // namespace

int main() {
  test_scope_raises_resolution();
  test_scopes_nest();
  std::cout << "timer resolution tests passed\n";
  return 0;
}
