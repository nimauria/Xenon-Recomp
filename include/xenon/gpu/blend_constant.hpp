#pragma once

#include <array>
#include <span>

#include "xenon/gpu/resource_ir.hpp"

namespace xenon::gpu {

// The blend constant a host whose colour blend has only a "blend factor"
// constant (D3D12's BLEND_FACTOR / INV_BLEND_FACTOR, which read the constant's
// RGB) must set so it reproduces the guest's constant factors.
//
// Xenos has separate CONSTANT_COLOR (the constant's RGB) and CONSTANT_ALPHA
// (its A, on every channel) factors. Mapping both to BLEND_FACTOR and passing
// the guest constant through made a colour slot asking for CONSTANT_ALPHA
// blend by the constant's red, green and blue instead - wrong opacity for any
// draw that fades with the blend constant's alpha.
//
// The alpha equation needs nothing: BLEND_FACTOR's alpha is the constant's
// alpha for both guest factors. A draw whose colour slots use CONSTANT_ALPHA
// and no CONSTANT_COLOR gets (a, a, a, a). One using both cannot be expressed
// with a single constant; `exact` is false for it and the constant is passed
// through unchanged (the caller counts it as unsupported).
struct HostBlendConstant {
  std::array<float, 4> value{};
  bool exact{true};
};

[[nodiscard]] constexpr bool is_constant_color_factor(BlendFactor factor) noexcept {
  return factor == BlendFactor::ConstantColor || factor == BlendFactor::InvConstantColor;
}

[[nodiscard]] constexpr bool is_constant_alpha_factor(BlendFactor factor) noexcept {
  return factor == BlendFactor::ConstantAlpha || factor == BlendFactor::InvConstantAlpha;
}

[[nodiscard]] constexpr HostBlendConstant shared_blend_factor_constant(
    std::span<const BlendState> blends, const std::array<float, 4>& constant) noexcept {
  bool colour_reads_alpha = false;
  bool colour_reads_colour = false;
  for (const auto& blend : blends) {
    if (!blend.enabled) continue;
    for (const auto factor : {blend.color_source, blend.color_destination}) {
      colour_reads_alpha = colour_reads_alpha || is_constant_alpha_factor(factor);
      colour_reads_colour = colour_reads_colour || is_constant_color_factor(factor);
    }
  }
  if (!colour_reads_alpha) return {constant, true};
  if (colour_reads_colour) return {constant, false};
  return {{constant[3], constant[3], constant[3], constant[3]}, true};
}

}  // namespace xenon::gpu
