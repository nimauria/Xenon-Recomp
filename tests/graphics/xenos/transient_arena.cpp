#include <cassert>
#include <iostream>

#include "xenon/gpu/transient_arena.hpp"

using xenon::gpu::TransientArena;

namespace {

// Regression: the arena restarted only when a draw was the batch's first
// command. A batch opened by a resolve or texture load kept the previous
// batch's offset, so the offset grew across batches until every indexed draw
// failed ("transient upload arena exhausted by one draw batch", 3761 draws in
// one AC6 session). Each batch now starts with the whole arena.
void test_new_batch_restarts_arena() {
  TransientArena arena{1024};
  assert(arena.allocate(1, 1000) == 0u);
  assert(!arena.fits(1, 100));
  // Batch 4 reuses this slot; nothing was allocated by its first commands.
  assert(arena.fits(4, 1024));
  assert(arena.allocate(4, 600) == 0u);
  assert(arena.allocate(4, 400) == 600u);
  std::cout << "  [PASS] a new batch restarts the arena\n";
}

void test_allocations_are_aligned_and_bounded() {
  TransientArena arena{64};
  assert(arena.allocate(7, 6) == 0u);
  assert(arena.allocate(7, 8) == 8u);  // 6 rounds up to 8
  assert(arena.fits(7, 48));
  assert(!arena.fits(7, 49));
  assert(!arena.allocate(7, 49));
  // A failed allocation leaves the batch's offset where it was.
  assert(arena.allocate(7, 48) == 16u);
  assert(!arena.fits(7, 1));
  assert(!arena.fits(8, 65));
  std::cout << "  [PASS] allocations are aligned and bounded by the capacity\n";
}

}  // namespace

int main() {
  std::cout << "Transient arena tests\n";
  test_new_batch_restarts_arena();
  test_allocations_are_aligned_and_bounded();
  return 0;
}
