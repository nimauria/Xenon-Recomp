#include "xenon/gpu/edram_ownership.hpp"

#include <algorithm>
#include <unordered_map>

namespace xenon::gpu {
namespace {

std::vector<std::uint16_t> covered_tiles(
    const EdramSurfaceLayout& surface) {
  return covered_edram_tiles(
      surface, {0, 0, int(surface.pitch_pixels), int(surface.height_pixels)});
}

EdramOwnershipPlan make_plan(
    EdramOwnerId requested_owner,
    const std::array<EdramOwnerId, Edram::kTileCount>& owners,
    std::vector<std::uint16_t> tiles) {
  EdramOwnershipPlan result{};
  result.requested_owner = requested_owner;
  if (requested_owner == kCanonicalEdramOwner) return result;
  result.covered_tiles = std::move(tiles);
  std::unordered_map<EdramOwnerId, std::size_t> change_indices;
  for (const auto tile : result.covered_tiles) {
    const auto previous = owners[tile];
    if (previous == requested_owner) continue;
    const auto [it, inserted] = change_indices.try_emplace(
        previous, result.changes.size());
    if (inserted) result.changes.push_back({previous, {}});
    result.changes[it->second].tiles.push_back(tile);
  }
  result.valid = !result.covered_tiles.empty();
  return result;
}

}  // namespace

bool EdramOwnershipPlan::requires_preservation() const noexcept {
  return std::any_of(changes.begin(), changes.end(), [](const auto& change) {
    return change.previous_owner != kCanonicalEdramOwner;
  });
}

void EdramOwnershipTracker::reset() noexcept {
  owners_.fill(kCanonicalEdramOwner);
  changed();
}

namespace {
std::uint64_t holding_key(EdramOwnerId owner, const EdramSurfaceLayout& surface) noexcept {
  return surface.hash() ^ (owner * 0x9E3779B97F4A7C15ull) ^ 0xC3A5C85C97CB3127ull;
}
}  // namespace

bool EdramOwnershipTracker::known_to_hold(EdramOwnerId owner,
                                          const EdramSurfaceLayout& surface) const noexcept {
  if (owner == kCanonicalEdramOwner) return false;
  const auto key = holding_key(owner, surface);
  for (const auto& holding : holdings_)
    if (holding.key == key && holding.generation == generation_) return true;
  return false;
}

void EdramOwnershipTracker::note_holds(EdramOwnerId owner,
                                       const EdramSurfaceLayout& surface) noexcept {
  if (owner == kCanonicalEdramOwner) return;
  const auto key = holding_key(owner, surface);
  for (auto& holding : holdings_) {
    if (holding.key == key) {
      holding.generation = generation_;
      return;
    }
  }
  holdings_[next_holding_] = {key, generation_};
  next_holding_ = (next_holding_ + 1u) % holdings_.size();
}

EdramOwnershipPlan EdramOwnershipTracker::plan(
    EdramOwnerId requested_owner, const EdramSurfaceLayout& surface) const {
  return make_plan(requested_owner, owners_, covered_tiles(surface));
}

EdramOwnershipPlan EdramOwnershipTracker::plan_region(
    EdramOwnerId requested_owner, const EdramSurfaceLayout& surface,
    EdramSurfaceRegion region) const {
  return make_plan(requested_owner, owners_,
                   covered_edram_tiles(surface, region));
}

bool EdramOwnershipTracker::commit(const EdramOwnershipPlan& plan) noexcept {
  if (!plan.valid || plan.requested_owner == kCanonicalEdramOwner) return false;
  // Reject stale plans rather than silently discarding a surface written after
  // planning but before the backend completed its preservation work.
  for (const auto& change : plan.changes)
    for (const auto tile : change.tiles)
      if (owners_[tile] != change.previous_owner) return false;
  for (const auto tile : plan.covered_tiles) owners_[tile] = plan.requested_owner;
  if (!plan.changes.empty()) changed();
  return true;
}

void EdramOwnershipTracker::make_canonical(
    const EdramSurfaceLayout& surface) noexcept {
  for (const auto tile : covered_tiles(surface))
    owners_[tile] = kCanonicalEdramOwner;
  changed();
}

void EdramOwnershipTracker::make_canonical_region(
    const EdramSurfaceLayout& surface, EdramSurfaceRegion region) noexcept {
  for (const auto tile : covered_edram_tiles(surface, region))
    owners_[tile] = kCanonicalEdramOwner;
  changed();
}

void EdramOwnershipTracker::release(EdramOwnerId owner_id) noexcept {
  if (owner_id == kCanonicalEdramOwner) return;
  for (auto& tile_owner : owners_)
    if (tile_owner == owner_id) tile_owner = kCanonicalEdramOwner;
  changed();
}

void EdramOwnershipTracker::release_tiles(EdramOwnerId owner_id,
                                          std::span<const std::uint16_t> tiles) noexcept {
  if (owner_id == kCanonicalEdramOwner) return;
  for (const auto tile : tiles) {
    auto& tile_owner = owners_[tile % Edram::kTileCount];
    if (tile_owner == owner_id) tile_owner = kCanonicalEdramOwner;
  }
  changed();
}

EdramOwnerId EdramOwnershipTracker::owner(std::uint32_t tile) const noexcept {
  return owners_[tile % Edram::kTileCount];
}

std::size_t EdramOwnershipTracker::owned_tile_count(
    EdramOwnerId owner_id) const noexcept {
  return static_cast<std::size_t>(std::count(owners_.begin(), owners_.end(),
                                             owner_id));
}

}  // namespace xenon::gpu
