#include <cassert>
#include <iostream>

#include "xenon/gpu/edram_ownership.hpp"

using namespace xenon::gpu;

int main() {
  EdramOwnershipTracker ownership;
  const EdramSurfaceLayout wrapped{2040, 1280, 32, MsaaSamples::X1,
                                    false, false};
  const auto first = ownership.plan(0x100, wrapped);
  assert(first.valid && !first.requires_preservation());
  assert(first.covered_tiles.size() == 32);
  assert(first.changes.size() == 1 &&
         first.changes[0].previous_owner == kCanonicalEdramOwner);
  assert(ownership.commit(first));
  assert(ownership.owned_tile_count(0x100) == 32);
  assert(ownership.owner(2040) == 0x100 && ownership.owner(7) == 0x100);

  const EdramSurfaceLayout overlap{0, 640, 16, MsaaSamples::X1,
                                    false, true};
  const auto second = ownership.plan(0x200, overlap);
  assert(second.valid && second.requires_preservation());
  assert(second.changes.size() == 1 &&
         second.changes[0].previous_owner == 0x100);
  assert(second.changes[0].tiles.size() == 8);
  assert(ownership.commit(second));
  assert(ownership.owner(0) == 0x200 && ownership.owner(8) == 0x100);

  // A plan is transactional and must fail if ownership changed meanwhile.
  const auto stale = ownership.plan(0x300, overlap);
  const auto intervening = ownership.plan(0x400, overlap);
  assert(ownership.commit(intervening));
  assert(!ownership.commit(stale));

  ownership.make_canonical(overlap);
  assert(ownership.owner(0) == kCanonicalEdramOwner);
  ownership.release(0x100);
  assert(ownership.owned_tile_count(0x100) == 0);

  // 64bpp consumes paired tiles and MSAA expands the sample-space footprint.
  const EdramSurfaceLayout wide_msaa{100, 80, 16, MsaaSamples::X4,
                                     true, false};
  const auto wide = ownership.plan(0x500, wide_msaa);
  assert(wide.valid && wide.covered_tiles.size() == 8);

  EdramOwnershipTracker regional;
  const auto left = regional.plan_region(0x600, wrapped, {0, 0, 80, 16});
  assert(left.valid && left.covered_tiles.size() == 1 &&
         left.covered_tiles[0] == 2040);
  assert(regional.commit(left));
  const auto right = regional.plan_region(0x700, wrapped,
                                          {1200, 0, 1280, 16});
  assert(right.valid && right.covered_tiles.size() == 1 &&
         right.covered_tiles[0] == 7);
  assert(regional.commit(right));
  regional.make_canonical_region(wrapped, {0, 0, 80, 16});
  assert(regional.owner(2040) == kCanonicalEdramOwner);
  assert(regional.owner(7) == 0x700);

  const auto wide_region_tiles = covered_edram_tiles(
      wide_msaa, {0, 0, 1, 1});
  assert(wide_region_tiles.size() == 2);

  std::cout << "xenon_edram_ownership_tests: ok\n";
}
