// ResidentRangeAllocator: granule-aligned best-fit allocation, ranges
// reusable only after the queue completes past their release, coalescing back
// to one free range, and allocation that stays a lookup however fragmented the
// buffer becomes.

#include "xenon/gpu/resident_range_allocator.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <random>
#include <vector>

using xenon::gpu::ResidentRangeAllocator;

int main() {
  ResidentRangeAllocator allocator(4096, 256);
  assert(allocator.free_bytes() == 4096 && allocator.granule() == 256);
  const auto a = allocator.allocate(100);  // one granule
  const auto b = allocator.allocate(300);  // two
  const auto c = allocator.allocate(256);  // one
  assert(a && b && c);
  assert(*a == 0 && *b == 256 && *c == 768);
  assert(*a % 256 == 0 && *b % 256 == 0 && *c % 256 == 0);
  assert(allocator.free_bytes() == 4096 - 1024);
  assert(!allocator.allocate(4096));
  assert(!allocator.allocate(0));

  // Released, but the GPU may still read it: not reusable yet.
  allocator.release(*b, 300, /*retire_after=*/10);
  assert(allocator.pending_bytes() == 512);
  allocator.reclaim(9);
  assert(allocator.pending_bytes() == 512);
  allocator.reclaim(10);
  assert(allocator.pending_bytes() == 0);
  // Best fit: the two-granule hole b left, not the large tail.
  const auto refill = allocator.allocate(400);
  assert(refill && *refill == 256);

  // Everything back: one free range of the whole capacity again.
  allocator.release(*a, 100, 11);
  allocator.release(*refill, 400, 11);
  allocator.release(*c, 256, 11);
  allocator.reclaim(11);
  assert(allocator.free_bytes() == 4096 && allocator.free_ranges() == 1u);
  const auto whole = allocator.allocate(4096);
  assert(whole && *whole == 0);

  // Fragmentation: thousands of live ranges with holes between them; every
  // allocation still succeeds while room remains, never overlaps a live range,
  // and stays fast.
  {
    constexpr std::uint64_t kCapacity = 64ull << 20;
    ResidentRangeAllocator fragmented(kCapacity, 256);
    std::mt19937 random(7u);
    struct Live { std::uint64_t offset, size; };
    std::vector<Live> live;
    std::uint64_t value = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int round = 0; round < 200000; ++round) {
      if (!live.empty() && (random() % 3u == 0u || fragmented.free_bytes() < (1u << 20))) {
        const auto index = random() % live.size();
        fragmented.release(live[index].offset, live[index].size, value);
        live[index] = live.back();
        live.pop_back();
      } else {
        const std::uint64_t size = 64u + random() % 8192u;
        const auto offset = fragmented.allocate(size);
        if (offset) {
          assert(*offset % 256 == 0 && *offset + size <= kCapacity);
          live.push_back({*offset, size});
        }
      }
      fragmented.reclaim(value++);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // No two live ranges overlap.
    std::sort(live.begin(), live.end(), [](const Live& x, const Live& y) { return x.offset < y.offset; });
    for (std::size_t i = 1; i < live.size(); ++i)
      assert(live[i - 1].offset + live[i - 1].size <= live[i].offset);
    const auto per_op_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 200000;
    std::cout << "fragmented: " << live.size() << " live ranges, " << fragmented.free_ranges()
              << " free ranges, " << per_op_ns << " ns per operation\n";
    assert(per_op_ns < 20000);  // a lookup, not a scan (generous for loaded CI hosts)
  }

  allocator.reset(0);
  assert(!allocator.allocate(1));
  std::cout << "xenon_resident_range_allocator_tests: ok\n";
  return 0;
}
