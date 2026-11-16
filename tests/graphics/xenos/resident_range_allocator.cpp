// ResidentRangeAllocator: aligned first-fit allocation, ranges reusable only
// after the queue completes past their release, and coalescing back to one
// free range.

#include "xenon/gpu/resident_range_allocator.hpp"

#include <cassert>
#include <iostream>

using xenon::gpu::ResidentRangeAllocator;

int main() {
  ResidentRangeAllocator allocator(1024);
  assert(allocator.free_bytes() == 1024);
  const auto a = allocator.allocate(100, 64);
  const auto b = allocator.allocate(100, 64);
  const auto c = allocator.allocate(200, 256);
  assert(a && b && c);
  assert(*a == 0 && *b == 128 && *c == 256 && *c % 256 == 0);
  // Gaps from alignment stay free (100..128 and 228..256).
  assert(allocator.free_bytes() == 1024 - 400);
  assert(!allocator.allocate(1024, 1));
  assert(!allocator.allocate(16, 3));  // alignment must be a power of two
  assert(!allocator.allocate(0, 4));

  // Released, but the GPU may still read it: not reusable yet.
  allocator.release(*c, 200, /*retire_after=*/10);
  assert(allocator.pending_bytes() == 200);
  const auto before = allocator.allocate(500, 256);
  assert(before && *before >= 456);  // past the pending range
  allocator.reclaim(9);
  assert(allocator.pending_bytes() == 200);
  allocator.reclaim(10);
  assert(allocator.pending_bytes() == 0);
  const auto reused = allocator.allocate(200, 256);
  assert(reused && *reused == 256);

  // Everything back: one free range of the whole capacity again.
  allocator.release(*a, 100, 11);
  allocator.release(*b, 100, 11);
  allocator.release(*reused, 200, 11);
  allocator.release(*before, 500, 11);
  allocator.reclaim(11);
  assert(allocator.free_bytes() == 1024 && allocator.pending_bytes() == 0);
  const auto whole = allocator.allocate(1024, 1024);
  assert(whole && *whole == 0);

  allocator.reset(0);
  assert(!allocator.allocate(1, 1));
  std::cout << "xenon_resident_range_allocator_tests: ok\n";
  return 0;
}
