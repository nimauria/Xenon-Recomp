#pragma once

// Xenos texture-fetch addressing in host terms, shared by the D3D12 and
// Vulkan backends.

#include <cstdint>

namespace xenon::gpu {

// The host address modes Xenos clamp modes map to.
enum class SamplerAddress : std::uint8_t {
  Wrap,
  Mirror,
  ClampToEdge,
  MirrorOnceToEdge,
  ClampToBorder,
};

// The fetch constant's 3-bit clamp field. Neither D3D12 nor Vulkan has the
// "halfway" modes (half edge, half border) or mirror-then-border, so those
// take their nearest host mode, as xenia does: halfway clamps to the edge
// and both mirror-once variants mirror once to the edge. Returns false for a
// value outside the field, which the caller counts as unsupported.
[[nodiscard]] constexpr bool xenos_sampler_address(std::uint8_t clamp,
                                                   SamplerAddress& out) noexcept {
  switch (clamp) {
    case 0: out = SamplerAddress::Wrap; return true;                // repeat
    case 1: out = SamplerAddress::Mirror; return true;              // mirrored repeat
    case 2: out = SamplerAddress::ClampToEdge; return true;         // clamp to last texel
    case 3: out = SamplerAddress::MirrorOnceToEdge; return true;    // mirror once to last texel
    case 4: out = SamplerAddress::ClampToEdge; return true;         // clamp halfway
    case 5: out = SamplerAddress::MirrorOnceToEdge; return true;    // mirror once halfway
    case 6: out = SamplerAddress::ClampToBorder; return true;       // clamp to border
    case 7: out = SamplerAddress::MirrorOnceToEdge; return true;    // mirror once to border
    default: return false;
  }
}

// The fetch constant's border colour: 1 is opaque white; the black variants
// (ABGR, and the two YCbCr orders) all read as transparent black.
[[nodiscard]] constexpr bool xenos_border_is_white(std::uint8_t border_color) noexcept {
  return border_color == 1u;
}

}  // namespace xenon::gpu
