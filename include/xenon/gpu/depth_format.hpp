#pragma once

#include <cstdint>

#include "xenon/gpu/edram_surface.hpp"

namespace xenon::gpu {

// Xenos D24FS8 uses a positive 24-bit floating depth value with a 20-bit
// mantissa and a 4-bit exponent (20e4). The representable range is [0, 2).
enum class Float20e4Rounding : std::uint8_t {
  Truncate,
  NearestEven,
};

inline constexpr std::uint32_t kFloat20e4Mask = 0x00FFFFFFu;

struct DepthStencilSample {
  float depth{};
  std::uint8_t stencil{};
};

[[nodiscard]] std::uint32_t float32_to_20e4(
    float value,
    Float20e4Rounding rounding = Float20e4Rounding::NearestEven) noexcept;
[[nodiscard]] float float20e4_to_float32(std::uint32_t value) noexcept;
[[nodiscard]] float quantize_float20e4(
    float value,
    Float20e4Rounding rounding = Float20e4Rounding::NearestEven) noexcept;

// Converts between the canonical 32-bit Xenos EDRAM representation and a
// native depth/stencil value. Keeping the packed word authoritative makes both
// D24S8 and D24FS8 reversible even when a host API exposes float32 depth.
[[nodiscard]] std::uint32_t pack_depth_stencil(
    DepthRenderTargetFormat format, float depth, std::uint8_t stencil,
    Float20e4Rounding rounding = Float20e4Rounding::NearestEven) noexcept;
[[nodiscard]] DepthStencilSample unpack_depth_stencil(
    DepthRenderTargetFormat format, std::uint32_t packed) noexcept;

// Maps canonical Xenos depth to the normalized range accepted by native depth
// attachments. D24FS8 uses [0,2), so it is scaled to [0,1) without losing any
// representable bits; the inverse restores the original 20e4 lattice value.
[[nodiscard]] DepthStencilSample depth_stencil_to_host(
    DepthRenderTargetFormat format, std::uint32_t packed) noexcept;
[[nodiscard]] std::uint32_t host_to_depth_stencil(
    DepthRenderTargetFormat format, float host_depth, std::uint8_t stencil,
    Float20e4Rounding rounding = Float20e4Rounding::NearestEven) noexcept;

}  // namespace xenon::gpu
