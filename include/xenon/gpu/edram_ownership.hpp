#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "xenon/gpu/edram_surface.hpp"

namespace xenon::gpu {

using EdramOwnerId = std::uint64_t;
inline constexpr EdramOwnerId kCanonicalEdramOwner = 0;

struct EdramOwnershipChange {
  EdramOwnerId previous_owner{};
  std::vector<std::uint16_t> tiles{};
};

struct EdramOwnershipPlan {
  EdramOwnerId requested_owner{};
  std::vector<std::uint16_t> covered_tiles{};
  std::vector<EdramOwnershipChange> changes{};
  bool valid{};

  [[nodiscard]] bool requires_preservation() const noexcept;
};

// Tracks which native surface (or the canonical 10 MiB byte store) contains
// the authoritative bits for every physical EDRAM tile. Planning is separate
// from committing so backends can finish all required readback/upload work
// before changing ownership.
class EdramOwnershipTracker {
 public:
  EdramOwnershipTracker() { reset(); }

  void reset() noexcept;
  [[nodiscard]] EdramOwnershipPlan plan(
      EdramOwnerId requested_owner,
      const EdramSurfaceLayout& surface) const;
  [[nodiscard]] EdramOwnershipPlan plan_region(
      EdramOwnerId requested_owner, const EdramSurfaceLayout& surface,
      EdramSurfaceRegion region) const;
  [[nodiscard]] bool commit(const EdramOwnershipPlan& plan) noexcept;
  void make_canonical(const EdramSurfaceLayout& surface) noexcept;
  void make_canonical_region(const EdramSurfaceLayout& surface,
                             EdramSurfaceRegion region) noexcept;
  void release(EdramOwnerId owner) noexcept;
  // Returns just `tiles` to canonical storage, where `owner` holds them (a
  // partial flush); the owner keeps the rest.
  void release_tiles(EdramOwnerId owner, std::span<const std::uint16_t> tiles) noexcept;

  [[nodiscard]] EdramOwnerId owner(std::uint32_t tile) const noexcept;
  [[nodiscard]] std::size_t owned_tile_count(EdramOwnerId owner) const noexcept;

  // Changes on every change of any tile's owner.
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
  // Whether `owner` is known to hold every tile `surface` covers: it did when
  // note_holds() was last called for this owner and surface, and no tile has
  // changed owner since. A draw that keeps rendering to the same target skips
  // planning (building the covered tile list and the changes) entirely.
  [[nodiscard]] bool known_to_hold(EdramOwnerId owner,
                                   const EdramSurfaceLayout& surface) const noexcept;
  // Records that `owner` holds every tile of `surface` now (after a commit, or
  // a plan with no changes).
  void note_holds(EdramOwnerId owner, const EdramSurfaceLayout& surface) noexcept;

 private:
  void changed() noexcept { ++generation_; }

  std::array<EdramOwnerId, Edram::kTileCount> owners_{};
  std::uint64_t generation_{1};
  // The latest confirmed holdings: (owner and surface key, generation).
  struct Holding {
    std::uint64_t key{};
    std::uint64_t generation{};
  };
  std::array<Holding, 16> holdings_{};
  std::size_t next_holding_{};
};

}  // namespace xenon::gpu
