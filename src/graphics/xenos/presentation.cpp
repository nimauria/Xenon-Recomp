#include "xenon/gpu/presentation.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#include "xenon/gpu/scanout.hpp"
#include "xenon/gpu/texture.hpp"

namespace xenon::gpu {
namespace {

using Rgba = std::array<float, 4>;

float unorm(std::uint32_t value, std::uint32_t max_value) {
  return float(value) / float(max_value);
}

float finite01(float value) {
  if (!std::isfinite(value)) return 0.0f;
  return std::clamp(value, 0.0f, 1.0f);
}

float half_to_float(std::uint16_t value) {
  const std::uint32_t sign = std::uint32_t(value & 0x8000u) << 16;
  std::uint32_t exponent = (value >> 10) & 0x1Fu;
  std::uint32_t mantissa = value & 0x03FFu;
  std::uint32_t bits{};
  if (exponent == 0) {
    if (!mantissa) {
      bits = sign;
    } else {
      exponent = 127u - 15u + 1u;
      while ((mantissa & 0x0400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x03FFu;
      bits = sign | (exponent << 23) | (mantissa << 13);
    }
  } else if (exponent == 0x1Fu) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + (127u - 15u)) << 23) | (mantissa << 13);
  }
  return std::bit_cast<float>(bits);
}

template <typename T, std::size_t N>
std::array<T, N> load(const std::byte* source) {
  std::array<T, N> values{};
  std::memcpy(values.data(), source, sizeof(T) * N);
  return values;
}

// One stored texel as RGBA in [0, 1]. Formats with fewer channels give
// (v, 0, 0, 1) and the like, before the fetch-constant swizzle.
bool convert_pixel(ScanoutDecode decode, const std::byte* source, Rgba& out) {
  switch (decode) {
    case ScanoutDecode::R8:
      out = {unorm(std::uint8_t(*source), 255u), 0.0f, 0.0f, 1.0f};
      return true;
    case ScanoutDecode::R8G8: {
      const auto v = load<std::uint8_t, 2>(source);
      out = {unorm(v[0], 255u), unorm(v[1], 255u), 0.0f, 1.0f};
      return true;
    }
    case ScanoutDecode::R8G8B8A8: {
      const auto v = load<std::uint8_t, 4>(source);
      for (unsigned i = 0; i < 4; ++i) out[i] = unorm(v[i], 255u);
      return true;
    }
    case ScanoutDecode::B5G5R5A1: {
      const auto v = load<std::uint16_t, 1>(source)[0];
      out = {unorm((v >> 10) & 31u, 31u), unorm((v >> 5) & 31u, 31u), unorm(v & 31u, 31u),
             (v & 0x8000u) ? 1.0f : 0.0f};
      return true;
    }
    case ScanoutDecode::B5G6R5: {
      const auto v = load<std::uint16_t, 1>(source)[0];
      out = {unorm((v >> 11) & 31u, 31u), unorm((v >> 5) & 63u, 63u), unorm(v & 31u, 31u),
             1.0f};
      return true;
    }
    case ScanoutDecode::B4G4R4A4: {
      const auto v = load<std::uint16_t, 1>(source)[0];
      out = {unorm((v >> 8) & 15u, 15u), unorm((v >> 4) & 15u, 15u), unorm(v & 15u, 15u),
             unorm((v >> 12) & 15u, 15u)};
      return true;
    }
    case ScanoutDecode::R10G10B10A2: {
      const auto v = load<std::uint32_t, 1>(source)[0];
      out = {unorm(v & 1023u, 1023u), unorm((v >> 10) & 1023u, 1023u),
             unorm((v >> 20) & 1023u, 1023u), unorm((v >> 30) & 3u, 3u)};
      return true;
    }
    case ScanoutDecode::R16:
      out = {unorm(load<std::uint16_t, 1>(source)[0], 65535u), 0.0f, 0.0f, 1.0f};
      return true;
    case ScanoutDecode::R16G16: {
      const auto v = load<std::uint16_t, 2>(source);
      out = {unorm(v[0], 65535u), unorm(v[1], 65535u), 0.0f, 1.0f};
      return true;
    }
    case ScanoutDecode::R16G16B16A16: {
      const auto v = load<std::uint16_t, 4>(source);
      for (unsigned i = 0; i < 4; ++i) out[i] = unorm(v[i], 65535u);
      return true;
    }
    case ScanoutDecode::R16Float:
      out = {finite01(half_to_float(load<std::uint16_t, 1>(source)[0])), 0.0f, 0.0f, 1.0f};
      return true;
    case ScanoutDecode::R16G16Float: {
      const auto v = load<std::uint16_t, 2>(source);
      out = {finite01(half_to_float(v[0])), finite01(half_to_float(v[1])), 0.0f, 1.0f};
      return true;
    }
    case ScanoutDecode::R16G16B16A16Float: {
      const auto v = load<std::uint16_t, 4>(source);
      for (unsigned i = 0; i < 4; ++i) out[i] = finite01(half_to_float(v[i]));
      return true;
    }
    case ScanoutDecode::R32Float:
      out = {finite01(load<float, 1>(source)[0]), 0.0f, 0.0f, 1.0f};
      return true;
    case ScanoutDecode::R32G32Float: {
      const auto v = load<float, 2>(source);
      out = {finite01(v[0]), finite01(v[1]), 0.0f, 1.0f};
      return true;
    }
    case ScanoutDecode::R32G32B32Float: {
      const auto v = load<float, 3>(source);
      out = {finite01(v[0]), finite01(v[1]), finite01(v[2]), 1.0f};
      return true;
    }
    case ScanoutDecode::R32G32B32A32Float: {
      const auto v = load<float, 4>(source);
      for (unsigned i = 0; i < 4; ++i) out[i] = finite01(v[i]);
      return true;
    }
  }
  return false;
}

// The front buffer is a texture, and scan-out reads it through its fetch
// constant's swizzle as sampling does (three bits per result component: 0-3
// select X/Y/Z/W, 4 forces 0, 5 forces 1). AC6's A8R8G8B8 front buffer is
// k_8_8_8_8 with red and blue swapped by the swizzle; ignoring it showed the
// Namco Bandai logo in blues.
Rgba apply_texture_swizzle(std::uint16_t swizzle, const Rgba& in) {
  Rgba out{};
  for (unsigned i = 0; i < 4; ++i) {
    const auto select = (swizzle >> (i * 3u)) & 7u;
    out[i] = select < 4u ? in[select] : select == 4u ? 0.0f : select == 5u ? 1.0f : in[i];
  }
  return out;
}

}  // namespace

PreparedPresentationFrame prepare_presentation_frame(
    const PresentationFrame& frame, std::span<const std::byte> physical_memory,
    std::uint32_t target_width, std::uint32_t target_height,
    bool preserve_aspect_ratio, std::uint32_t physical_base) {
  PreparedPresentationFrame output{};
  const auto plan =
      plan_scanout(frame, target_width, target_height, preserve_aspect_ratio);
  if (!plan.valid) {
    output.error = plan.error;
    return output;
  }
  const auto& c = plan.constants;

  TextureDescriptor source_descriptor = frame.texture;
  source_descriptor.mip_min_level = 0;
  source_descriptor.mip_max_level = 0;
  source_descriptor.packed_mips = false;
  const auto decoded =
      decode_texture(source_descriptor, physical_memory, physical_base);
  if (!decoded.valid || decoded.layout.subresources.empty()) {
    output.error = decoded.error.empty() ? "scanout texture decode failed" : decoded.error;
    return output;
  }
  const auto& subresource = decoded.layout.subresources.front();
  const auto decode = static_cast<ScanoutDecode>(c.decode);
  std::vector<Rgba> source(std::size_t(c.source_width) * c.source_height);
  const auto source_base = decoded.linear_data.data() + subresource.linear_offset_bytes;
  for (std::uint32_t y = 0; y < c.source_height; ++y) {
    const auto* row = source_base + std::size_t(y) * subresource.linear_row_pitch_bytes;
    for (std::uint32_t x = 0; x < c.source_width; ++x) {
      Rgba texel{};
      if (!convert_pixel(decode, row + std::size_t(x) * c.bytes_per_block, texel)) {
        output.error = "presentation source format has no RGBA8 scanout conversion";
        return output;
      }
      source[std::size_t(y) * c.source_width + x] =
          apply_texture_swizzle(source_descriptor.swizzle, texel);
    }
  }

  output.width = target_width;
  output.height = target_height;
  output.row_pitch = target_width * 4u;
  output.rgba8.assign(std::size_t(output.row_pitch) * target_height, std::byte{});

  // Catmull-Rom bicubic, the CPU reference for make_scanout_shader(): same
  // taps, weights and summation order. Scan-out isn't part of guest rendering
  // semantics, so the filter affects host output quality only.
  for (std::uint32_t y = 0; y < c.draw_height; ++y) {
    const auto ty = scanout_taps(c.offset_y + std::int32_t(y), c.offset_y, c.draw_height,
                                 c.source_height);
    for (std::uint32_t x = 0; x < c.draw_width; ++x) {
      const auto tx = scanout_taps(c.offset_x + std::int32_t(x), c.offset_x, c.draw_width,
                                   c.source_width);
      Rgba sum{};
      for (int j = 0; j < 4; ++j) {
        if (ty.weights[j] == 0.0f) continue;
        const auto sy = std::clamp(ty.first + j, 0, std::int32_t(c.source_height) - 1);
        Rgba row{};
        for (int i = 0; i < 4; ++i) {
          if (tx.weights[i] == 0.0f) continue;
          const auto sx = std::clamp(tx.first + i, 0, std::int32_t(c.source_width) - 1);
          const auto& texel = source[std::size_t(sy) * c.source_width + std::size_t(sx)];
          for (unsigned k = 0; k < 4; ++k) row[k] += tx.weights[i] * texel[k];
        }
        for (unsigned k = 0; k < 4; ++k) sum[k] += ty.weights[j] * row[k];
      }
      auto* dst = reinterpret_cast<std::uint8_t*>(
          output.rgba8.data() + std::size_t(c.offset_y + y) * output.row_pitch +
          std::size_t(c.offset_x + x) * 4u);
      for (unsigned k = 0; k < 4; ++k)
        dst[k] = static_cast<std::uint8_t>(std::lround(std::clamp(sum[k], 0.0f, 1.0f) * 255.0f));
    }
  }
  output.valid = true;
  return output;
}

}  // namespace xenon::gpu
