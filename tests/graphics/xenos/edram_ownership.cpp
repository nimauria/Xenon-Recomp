#include <cassert>
#include <iostream>

#include "xenon/gpu/edram_ownership.hpp"

using namespace xenon::gpu;

namespace {

// A partial flush returns only the given tiles, and only where the named
// owner holds them; the owner keeps every other tile.
void test_release_tiles_is_partial() {
  EdramOwnershipTracker ownership;
  const EdramSurfaceLayout surface{0, 1280, 64, MsaaSamples::X1, false, false};
  const auto plan = ownership.plan(0x7, surface);
  assert(ownership.commit(plan));
  const auto held = ownership.owned_tile_count(0x7);
  assert(held == plan.covered_tiles.size() && held > 4u);
  const std::uint16_t returned[] = {plan.covered_tiles[0], plan.covered_tiles[1], 2047u};
  ownership.release_tiles(0x7, returned);
  assert(ownership.owner(plan.covered_tiles[0]) == kCanonicalEdramOwner);
  assert(ownership.owner(plan.covered_tiles[1]) == kCanonicalEdramOwner);
  assert(ownership.owner(plan.covered_tiles[2]) == 0x7);
  assert(ownership.owned_tile_count(0x7) == held - 2u);
  // Another owner releasing tiles it does not hold changes nothing.
  ownership.release_tiles(0x9, plan.covered_tiles);
  assert(ownership.owned_tile_count(0x7) == held - 2u);
}

// The per-draw fast path: an owner noted as holding a surface is known to
// hold it until any tile changes owner - another owner taking tiles, a
// release, a partial release, making tiles canonical or a reset - and only
// for that exact surface (a taller one covers more tiles).
void test_known_holdings_end_on_any_owner_change() {
  EdramOwnershipTracker ownership;
  const EdramSurfaceLayout surface{0, 640, 32, MsaaSamples::X1, false, false};
  const EdramSurfaceLayout taller{0, 640, 64, MsaaSamples::X1, false, false};
  const EdramSurfaceLayout elsewhere{1000, 640, 32, MsaaSamples::X1, false, false};
  assert(!ownership.known_to_hold(0x7, surface));
  const auto take = [&](EdramOwnerId owner, const EdramSurfaceLayout& s) {
    const auto plan = ownership.plan(owner, s);
    assert(plan.valid && ownership.commit(plan));
    ownership.note_holds(owner, s);
  };
  take(0x7, surface);
  assert(ownership.known_to_hold(0x7, surface));
  assert(!ownership.known_to_hold(0x7, taller));
  assert(!ownership.known_to_hold(0x8, surface));
  // Re-taking what it already holds changes no owner: still known.
  const auto again = ownership.plan(0x7, surface);
  assert(again.valid && again.changes.empty() && ownership.commit(again));
  assert(ownership.known_to_hold(0x7, surface));

  // Any owner change anywhere ends what is known (it could be one of these
  // tiles), even one on tiles far away.
  take(0x9, elsewhere);
  assert(!ownership.known_to_hold(0x7, surface));
  ownership.note_holds(0x7, surface);
  assert(ownership.known_to_hold(0x7, surface));

  const std::uint16_t one[] = {0u};
  ownership.release_tiles(0x7, one);
  assert(!ownership.known_to_hold(0x7, surface));
  take(0x7, surface);
  ownership.make_canonical_region(surface, {0, 0, 80, 16});
  assert(!ownership.known_to_hold(0x7, surface));
  take(0x7, surface);
  ownership.make_canonical(elsewhere);
  assert(!ownership.known_to_hold(0x7, surface));
  take(0x7, surface);
  ownership.release(0x9);
  assert(!ownership.known_to_hold(0x7, surface));
  take(0x7, surface);
  ownership.reset();
  assert(!ownership.known_to_hold(0x7, surface));
  // The canonical store is never "known" to hold anything.
  ownership.note_holds(kCanonicalEdramOwner, surface);
  assert(!ownership.known_to_hold(kCanonicalEdramOwner, surface));
}

}  // namespace

int main() {
  test_release_tiles_is_partial();
  test_known_holdings_end_on_any_owner_change();
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
