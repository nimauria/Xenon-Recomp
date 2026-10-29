#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "xenon/gpu/staging_ring.hpp"

namespace {

using xenon::gpu::StagingRing;

// A queue whose completed value only advances when the ring waits on it.
struct FakeQueue {
  std::uint64_t completed{};
  std::vector<std::uint64_t> waits{};
  bool fail{};
  bool wait(std::uint64_t value) {
    waits.push_back(value);
    if (fail) return false;
    if (value > completed) completed = value;
    return true;
  }
};

// Uploads that fit behind the in-flight ones never wait.
void test_reservations_do_not_wait_while_space_remains() {
  StagingRing ring;
  ring.reset(4096);
  FakeQueue queue;
  const auto wait = [&](std::uint64_t value) { return queue.wait(value); };
  const auto a = ring.reserve(1000, queue.completed, wait);
  assert(a && *a == 0);
  ring.commit(*a, 1000, 1);
  const auto b = ring.reserve(1000, queue.completed, wait);
  assert(b && *b == 1024);  // 256-byte aligned after the first region
  ring.commit(*b, 1000, 2);
  assert(queue.waits.empty() && ring.in_flight() == 2);
}

// Wrapping onto a region the GPU has not consumed waits for exactly the
// values protecting the overwritten regions, and no further.
void test_wrap_waits_only_for_overwritten_regions() {
  StagingRing ring;
  ring.reset(4096);
  FakeQueue queue;
  const auto wait = [&](std::uint64_t value) { return queue.wait(value); };
  for (std::uint64_t value = 1; value <= 4; ++value) {
    const auto offset = ring.reserve(1024, queue.completed, wait);
    assert(offset && *offset == (value - 1) * 1024);
    ring.commit(*offset, 1024, value);
  }
  assert(queue.waits.empty());
  const auto wrapped = ring.reserve(1500, queue.completed, wait);
  assert(wrapped && *wrapped == 0);
  assert(queue.waits.size() == 1 && queue.waits[0] == 2);
  ring.commit(*wrapped, 1500, 5);
  assert(ring.in_flight() == 3);  // regions 3, 4 and the new one
}

// A region the queue already completed is reused without waiting.
void test_completed_regions_are_reused_without_waiting() {
  StagingRing ring;
  ring.reset(2048);
  FakeQueue queue;
  const auto wait = [&](std::uint64_t value) { return queue.wait(value); };
  auto offset = ring.reserve(2048, queue.completed, wait);
  ring.commit(*offset, 2048, 7);
  queue.completed = 7;
  offset = ring.reserve(512, queue.completed, wait);
  assert(offset && *offset == 0 && queue.waits.empty());
}

void test_invalid_reservations_and_wait_failure() {
  StagingRing ring;
  ring.reset(1024);
  FakeQueue queue;
  const auto wait = [&](std::uint64_t value) { return queue.wait(value); };
  assert(!ring.reserve(0, 0, wait));
  assert(!ring.reserve(1025, 0, wait));
  auto offset = ring.reserve(1024, 0, wait);
  ring.commit(*offset, 1024, 3);
  queue.fail = true;
  assert(!ring.reserve(16, queue.completed, wait));
}

}  // namespace

int main() {
  test_reservations_do_not_wait_while_space_remains();
  test_wrap_waits_only_for_overwritten_regions();
  test_completed_regions_are_reused_without_waiting();
  test_invalid_reservations_and_wait_failure();
  std::cout << "xenon_staging_ring_tests: ok\n";
}
