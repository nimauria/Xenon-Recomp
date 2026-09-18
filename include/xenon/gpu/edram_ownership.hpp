#pragma once

#include <array>
#include <cstdint>
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

  [[nodiscard]] EdramOwnerId owner(std::uint32_t tile) const noexcept;
  [[nodiscard]] std::size_t owned_tile_count(EdramOwnerId owner) const noexcept;

 private:
  std::array<EdramOwnerId, Edram::kTileCount> owners_{};
};

}  // namespace xenon::gpu
