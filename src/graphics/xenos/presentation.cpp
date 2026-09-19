#include "xenon/gpu/presentation.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#include "xenon/gpu/texture.hpp"

namespace xenon::gpu {
namespace {

std::uint8_t unorm_to_u8(std::uint32_t value, std::uint32_t max_value) {
  if (!max_value) return 0;
  return static_cast<std::uint8_t>((value * 255u + max_value / 2u) / max_value);
}

std::uint8_t float_to_u8(float value) {
  if (!std::isfinite(value)) value = 0.0f;
  value = std::clamp(value, 0.0f, 1.0f);
  return static_cast<std::uint8_t>(std::lround(value * 255.0f));
}

float half_to_float(std::uint16_t value) {
#if defined(__cpp_lib_bit_cast)
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
#else
  (void)value;
  return 0.0f;
#endif
}

bool convert_pixel(TextureHostFormat format, const std::byte* source,
                   std::byte* destination) {
  auto* out = reinterpret_cast<std::uint8_t*>(destination);
  switch (format) {
    case TextureHostFormat::R8Unorm: {
      const auto value = static_cast<std::uint8_t>(*source);
      out[0] = out[1] = out[2] = value;
      out[3] = 255u;
      return true;
    }
    case TextureHostFormat::R8G8Unorm: {
      const auto* value = reinterpret_cast<const std::uint8_t*>(source);
      out[0] = value[0];
      out[1] = value[1];
      out[2] = 0u;
      out[3] = 255u;
      return true;
    }
    case TextureHostFormat::R8G8B8A8Unorm:
    case TextureHostFormat::R8G8B8A8Srgb:
      std::memcpy(out, source, 4);
      return true;
    case TextureHostFormat::B5G5R5A1Unorm: {
      std::uint16_t v{};
      std::memcpy(&v, source, sizeof(v));
      out[0] = unorm_to_u8((v >> 10) & 31u, 31u);
      out[1] = unorm_to_u8((v >> 5) & 31u, 31u);
      out[2] = unorm_to_u8(v & 31u, 31u);
      out[3] = (v & 0x8000u) ? 255u : 0u;
      return true;
    }
    case TextureHostFormat::B5G6R5Unorm: {
      std::uint16_t v{};
      std::memcpy(&v, source, sizeof(v));
      out[0] = unorm_to_u8((v >> 11) & 31u, 31u);
      out[1] = unorm_to_u8((v >> 5) & 63u, 63u);
      out[2] = unorm_to_u8(v & 31u, 31u);
      out[3] = 255u;
      return true;
    }
    case TextureHostFormat::B4G4R4A4Unorm: {
      std::uint16_t v{};
      std::memcpy(&v, source, sizeof(v));
      out[0] = unorm_to_u8((v >> 8) & 15u, 15u);
      out[1] = unorm_to_u8((v >> 4) & 15u, 15u);
      out[2] = unorm_to_u8(v & 15u, 15u);
      out[3] = unorm_to_u8((v >> 12) & 15u, 15u);
      return true;
    }
    case TextureHostFormat::R10G10B10A2Unorm: {
      std::uint32_t v{};
      std::memcpy(&v, source, sizeof(v));
      out[0] = unorm_to_u8(v & 1023u, 1023u);
      out[1] = unorm_to_u8((v >> 10) & 1023u, 1023u);
      out[2] = unorm_to_u8((v >> 20) & 1023u, 1023u);
      out[3] = unorm_to_u8((v >> 30) & 3u, 3u);
      return true;
    }
    case TextureHostFormat::R16Unorm: {
      std::uint16_t v{};
      std::memcpy(&v, source, sizeof(v));
      out[0] = out[1] = out[2] = unorm_to_u8(v, 65535u);
      out[3] = 255u;
      return true;
    }
    case TextureHostFormat::R16G16Unorm: {
      std::uint16_t v[2]{};
      std::memcpy(v, source, sizeof(v));
      out[0] = unorm_to_u8(v[0], 65535u);
      out[1] = unorm_to_u8(v[1], 65535u);
      out[2] = 0u;
      out[3] = 255u;
      return true;
    }
    case TextureHostFormat::R16G16B16A16Unorm: {
      std::uint16_t v[4]{};
      std::memcpy(v, source, sizeof(v));
      for (unsigned i = 0; i < 4; ++i) out[i] = unorm_to_u8(v[i], 65535u);
      return true;
    }
    case TextureHostFormat::R16Float: {
      std::uint16_t v{};
      std::memcpy(&v, source, sizeof(v));
      out[0] = out[1] = out[2] = float_to_u8(half_to_float(v));
      out[3] = 255u;
      return true;
    }
    case TextureHostFormat::R16G16Float: {
      std::uint16_t v[2]{};
      std::memcpy(v, source, sizeof(v));
      out[0] = float_to_u8(half_to_float(v[0]));
      out[1] = float_to_u8(half_to_float(v[1]));
      out[2] = 0u;
      out[3] = 255u;
      return true;
    }
    case TextureHostFormat::R16G16B16A16Float: {
      std::uint16_t v[4]{};
      std::memcpy(v, source, sizeof(v));
      for (unsigned i = 0; i < 4; ++i) out[i] = float_to_u8(half_to_float(v[i]));
      return true;
    }
    case TextureHostFormat::R32Float: {
      float v{};
      std::memcpy(&v, source, sizeof(v));
      out[0] = out[1] = out[2] = float_to_u8(v);
      out[3] = 255u;
      return true;
    }
    case TextureHostFormat::R32G32Float: {
      float v[2]{};
      std::memcpy(v, source, sizeof(v));
      out[0] = float_to_u8(v[0]);
      out[1] = float_to_u8(v[1]);
      out[2] = 0u;
      out[3] = 255u;
      return true;
    }
    case TextureHostFormat::R32G32B32Float: {
      float v[3]{};
      std::memcpy(v, source, sizeof(v));
      out[0] = float_to_u8(v[0]);
      out[1] = float_to_u8(v[1]);
      out[2] = float_to_u8(v[2]);
      out[3] = 255u;
      return true;
    }
    case TextureHostFormat::R32G32B32A32Float: {
      float v[4]{};
      std::memcpy(v, source, sizeof(v));
      for (unsigned i = 0; i < 4; ++i) out[i] = float_to_u8(v[i]);
      return true;
    }
    default:
      return false;
  }
}

}  // namespace

PreparedPresentationFrame prepare_presentation_frame(
    const PresentationFrame& frame, std::span<const std::byte> physical_memory,
    std::uint32_t target_width, std::uint32_t target_height,
    bool preserve_aspect_ratio, std::uint32_t physical_base) {
  PreparedPresentationFrame output{};
  if (!frame.texture.valid || !target_width || !target_height) {
    output.error = "presentation requires a valid texture and non-zero host extent";
    return output;
  }
  if (frame.texture.dimension != TextureDimension::TwoDOrStacked ||
      frame.texture.stacked || frame.texture.depth != 1) {
    output.error = "presentation currently requires a non-stacked 2D scanout texture";
    return output;
  }

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
  const std::uint32_t source_width = std::min(
      frame.visible_width ? frame.visible_width : source_descriptor.width,
      subresource.width_texels);
  const std::uint32_t source_height = std::min(
      frame.visible_height ? frame.visible_height : source_descriptor.height,
      subresource.height_texels);
  if (!source_width || !source_height || decoded.layout.format.storage != TextureStorage::Uncompressed) {
    output.error = "presentation requires a non-empty uncompressed base texture";
    return output;
  }
  const auto bytes_per_pixel = decoded.layout.format.bytes_per_block();
  if (!bytes_per_pixel || bytes_per_pixel > 16) {
    output.error = "presentation source pixel size is unsupported";
    return output;
  }

  std::vector<std::byte> source_rgba(
      std::size_t(source_width) * source_height * 4u);
  const auto source_base = decoded.linear_data.data() + subresource.linear_offset_bytes;
  for (std::uint32_t y = 0; y < source_height; ++y) {
    const auto* row = source_base + std::size_t(y) * subresource.linear_row_pitch_bytes;
    for (std::uint32_t x = 0; x < source_width; ++x) {
      if (!convert_pixel(decoded.layout.format.host_format,
                         row + std::size_t(x) * bytes_per_pixel,
                         source_rgba.data() +
                             (std::size_t(y) * source_width + x) * 4u)) {
        output.error = "presentation source format has no RGBA8 scanout conversion";
        return output;
      }
    }
  }

  output.width = target_width;
  output.height = target_height;
  output.row_pitch = target_width * 4u;
  output.rgba8.assign(std::size_t(output.row_pitch) * target_height, std::byte{});

  std::uint32_t draw_width = target_width;
  std::uint32_t draw_height = target_height;
  std::uint32_t offset_x{};
  std::uint32_t offset_y{};
  if (preserve_aspect_ratio) {
    const double sx = double(target_width) / double(source_width);
    const double sy = double(target_height) / double(source_height);
    const double scale = std::min(sx, sy);
    draw_width = std::max(1u, static_cast<std::uint32_t>(std::lround(source_width * scale)));
    draw_height = std::max(1u, static_cast<std::uint32_t>(std::lround(source_height * scale)));
    draw_width = std::min(draw_width, target_width);
    draw_height = std::min(draw_height, target_height);
    offset_x = (target_width - draw_width) / 2u;
    offset_y = (target_height - draw_height) / 2u;
  }

  // Bilinear scale. Presentation isn't part of guest rendering semantics, so
  // this filter affects only host scanout quality, never emulated texture data.
  for (std::uint32_t y = 0; y < draw_height; ++y) {
    const float source_y = std::clamp(
        (float(y) + 0.5f) * float(source_height) / float(draw_height) - 0.5f,
        0.0f, float(source_height - 1u));
    const auto y0 = static_cast<std::uint32_t>(std::floor(source_y));
    const auto y1 = std::min(y0 + 1u, source_height - 1u);
    const float fy = source_y - float(y0);
    for (std::uint32_t x = 0; x < draw_width; ++x) {
      const float source_x = std::clamp(
          (float(x) + 0.5f) * float(source_width) / float(draw_width) - 0.5f,
          0.0f, float(source_width - 1u));
      const auto x0 = static_cast<std::uint32_t>(std::floor(source_x));
      const auto x1 = std::min(x0 + 1u, source_width - 1u);
      const float fx = source_x - float(x0);
      auto* dst = reinterpret_cast<std::uint8_t*>(
          output.rgba8.data() + std::size_t(offset_y + y) * output.row_pitch +
          std::size_t(offset_x + x) * 4u);
      const auto* p00 = reinterpret_cast<const std::uint8_t*>(
          source_rgba.data() + (std::size_t(y0) * source_width + x0) * 4u);
      const auto* p10 = reinterpret_cast<const std::uint8_t*>(
          source_rgba.data() + (std::size_t(y0) * source_width + x1) * 4u);
      const auto* p01 = reinterpret_cast<const std::uint8_t*>(
          source_rgba.data() + (std::size_t(y1) * source_width + x0) * 4u);
      const auto* p11 = reinterpret_cast<const std::uint8_t*>(
          source_rgba.data() + (std::size_t(y1) * source_width + x1) * 4u);
      for (unsigned c = 0; c < 4; ++c) {
        const float top = float(p00[c]) + (float(p10[c]) - float(p00[c])) * fx;
        const float bottom = float(p01[c]) + (float(p11[c]) - float(p01[c])) * fx;
        dst[c] = static_cast<std::uint8_t>(std::clamp(
            std::lround(top + (bottom - top) * fy), 0l, 255l));
      }
    }
  }
  output.valid = true;
  return output;
}

}  // namespace xenon::gpu
