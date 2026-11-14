#include "xenon/gpu/depth_format.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>

namespace xenon::gpu {

std::uint32_t float32_to_20e4(float value, Float20e4Rounding rounding) noexcept {
  // Xenos depth is positive-only. This deliberately maps -0, negatives and NaN
  // to zero, matching the guest conversion behaviour used by Xbox 360 D3D.
  if (!(value > 0.0f)) {
    return 0;
  }

  auto bits = std::bit_cast<std::uint32_t>(value);

  // 0x3FFFFFF8 is the float32 representation of the largest 20e4 value after
  // round-trip expansion. Anything at or above it saturates to the 24-bit max.
  if (bits >= 0x3FFFFFF8u) {
    return kFloat20e4Mask;
  }

  // Convert the IEEE-754 8-bit exponent to Xenos' 4-bit exponent. Values below
  // the first normalized 20e4 exponent are shifted into the 20-bit denormal
  // mantissa instead of being flushed.
  if (bits < 0x38800000u) {
    const auto shift = std::min<std::uint32_t>(113u - (bits >> 23u), 24u);
    bits = (0x00800000u | (bits & 0x007FFFFFu)) >> shift;
  } else {
    // Bias adjustment from float32 exponent 127 to 20e4 exponent 15, performed
    // in the packed representation before the final 3-bit mantissa reduction.
    bits += 0xC8000000u;
  }

  if (rounding == Float20e4Rounding::NearestEven) {
    // Discarding 3 float32 mantissa bits. +3 rounds below half down, above half
    // up; the retained low bit provides the tie-to-even correction.
    bits += 3u + ((bits >> 3u) & 1u);
  }

  return (bits >> 3u) & kFloat20e4Mask;
}

float float20e4_to_float32(std::uint32_t value) noexcept {
  value &= kFloat20e4Mask;
  if (!value) {
    return 0.0f;
  }

  std::uint32_t mantissa = value & 0x000FFFFFu;
  std::int32_t exponent = static_cast<std::int32_t>(value >> 20u);
  if (!exponent) {
    // Normalize the 20e4 denormal. The subtraction converts countl_zero on a
    // 32-bit integer into the leading-zero count of the 21-bit value consisting
    // of the implicit leading one plus the 20-bit mantissa field.
    const auto shift = std::countl_zero(mantissa) - (32u - 21u);
    exponent = 1 - static_cast<std::int32_t>(shift);
    mantissa = (mantissa << shift) & 0x000FFFFFu;
  }

  const auto ieee = (static_cast<std::uint32_t>(exponent + 112) << 23u) |
                    (mantissa << 3u);
  return std::bit_cast<float>(ieee);
}

float quantize_float20e4(float value, Float20e4Rounding rounding) noexcept {
  return float20e4_to_float32(float32_to_20e4(value, rounding));
}

std::uint32_t pack_depth_stencil(DepthRenderTargetFormat format, float depth,
                                 std::uint8_t stencil,
                                 Float20e4Rounding rounding) noexcept {
  std::uint32_t depth_bits{};
  if (format == DepthRenderTargetFormat::D24FS8) {
    depth_bits = float32_to_20e4(depth, rounding);
  } else if (depth > 0.0f) {
    const auto clamped = std::min(depth, 1.0f);
    depth_bits = static_cast<std::uint32_t>(
        std::nearbyint(static_cast<double>(clamped) * 16777215.0));
  }
  return (depth_bits << 8u) | stencil;
}

DepthStencilSample unpack_depth_stencil(DepthRenderTargetFormat format,
                                        std::uint32_t packed) noexcept {
  const auto depth_bits = packed >> 8u;
  DepthStencilSample result{};
  result.depth = format == DepthRenderTargetFormat::D24FS8
                     ? float20e4_to_float32(depth_bits)
                     : float(depth_bits + (depth_bits >> 23u)) *
                           (1.0f / float(1u << 24u));
  result.stencil = static_cast<std::uint8_t>(packed & 0xFFu);
  return result;
}

std::string_view unorm24_depth_write_name(Unorm24DepthWrite write) noexcept {
  switch (write) {
    case Unorm24DepthWrite::Midpoint: return "midpoint";
    case Unorm24DepthWrite::PowerOfTwo: return "power-of-two";
    case Unorm24DepthWrite::Split: return "split";
  }
  return "unknown";
}

float unorm24_depth_for_write(std::uint32_t code, Unorm24DepthWrite write) noexcept {
  code &= 0xFFFFFFu;
  // Integer to float rounds to nearest even here and in the HLSL alike; the
  // power-of-two scale is exact.
  if (write == Unorm24DepthWrite::PowerOfTwo ||
      (write == Unorm24DepthWrite::Split && code < 0x800000u))
    return float(code) * (1.0f / 16777216.0f);
  return float(2u * code + 1u + (code >> 23u)) * (1.0f / 33554432.0f);
}

std::vector<std::uint32_t> unorm24_depth_calibration_words(std::uint32_t count) {
  constexpr std::array<std::uint32_t, 10> kEdges{0u,        1u,        2u,        0x3FFFFFu,
                                                 0x7FFFFEu, 0x7FFFFFu, 0x800000u, 0x800001u,
                                                 0xFFFFFEu, 0xFFFFFFu};
  count = std::max<std::uint32_t>(count, 16u);
  std::vector<std::uint32_t> words;
  words.reserve(count);
  for (const auto code : kEdges) words.push_back(code);
  const auto spread = count - std::uint32_t(kEdges.size());
  for (std::uint32_t i = 0; i < spread; ++i) {
    // Evenly over [0, 0xFFFFFF], with the low bits stirred so every residue
    // of a code's low byte turns up.
    const auto even = std::uint32_t(std::uint64_t(i) * 0xFFFFFFu / (spread - 1u));
    words.push_back(std::min<std::uint32_t>(even ^ ((i * 0x9Du) & 0x3Fu), 0xFFFFFFu));
  }
  for (std::uint32_t i = 0; i < count; ++i)
    words[i] = (words[i] << 8u) | ((i * 0x5Bu + 0x11u) & 0xFFu);
  return words;
}

DepthStencilSample depth_stencil_to_host(DepthRenderTargetFormat format, std::uint32_t packed,
                                         Unorm24DepthWrite unorm24_write) noexcept {
  auto result = unpack_depth_stencil(format, packed);
  if (format == DepthRenderTargetFormat::D24FS8) result.depth *= 0.5f;
  else result.depth = unorm24_depth_for_write(packed >> 8u, unorm24_write);
  return result;
}

std::uint32_t host_to_depth_stencil(DepthRenderTargetFormat format,
                                    float host_depth,
                                    std::uint8_t stencil,
                                    Float20e4Rounding rounding) noexcept {
  if (format == DepthRenderTargetFormat::D24FS8) host_depth *= 2.0f;
  return pack_depth_stencil(format, host_depth, stencil, rounding);
}

float host_viewport_depth(DepthRenderTargetFormat format, float guest_depth) noexcept {
  if (format != DepthRenderTargetFormat::D24FS8) return guest_depth;
  return quantize_float20e4(guest_depth) * 0.5f;
}

}  // namespace xenon::gpu
