#include "xenon/gpu/scanout.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>

#include "xenon/gpu/texture.hpp"

namespace xenon::gpu {
namespace {

std::optional<ScanoutDecode> scanout_decode(TextureHostFormat format) noexcept {
  switch (format) {
    case TextureHostFormat::R8Unorm: return ScanoutDecode::R8;
    case TextureHostFormat::R8G8Unorm: return ScanoutDecode::R8G8;
    // The front buffer's bytes are scanned out as stored; a gamma format is
    // not re-encoded on the way to the display.
    case TextureHostFormat::R8G8B8A8Unorm:
    case TextureHostFormat::R8G8B8A8Srgb: return ScanoutDecode::R8G8B8A8;
    case TextureHostFormat::B5G5R5A1Unorm: return ScanoutDecode::B5G5R5A1;
    case TextureHostFormat::B5G6R5Unorm: return ScanoutDecode::B5G6R5;
    case TextureHostFormat::B4G4R4A4Unorm: return ScanoutDecode::B4G4R4A4;
    case TextureHostFormat::R10G10B10A2Unorm: return ScanoutDecode::R10G10B10A2;
    case TextureHostFormat::R16Unorm: return ScanoutDecode::R16;
    case TextureHostFormat::R16G16Unorm: return ScanoutDecode::R16G16;
    case TextureHostFormat::R16G16B16A16Unorm: return ScanoutDecode::R16G16B16A16;
    case TextureHostFormat::R16Float: return ScanoutDecode::R16Float;
    case TextureHostFormat::R16G16Float: return ScanoutDecode::R16G16Float;
    case TextureHostFormat::R16G16B16A16Float: return ScanoutDecode::R16G16B16A16Float;
    case TextureHostFormat::R32Float: return ScanoutDecode::R32Float;
    case TextureHostFormat::R32G32Float: return ScanoutDecode::R32G32Float;
    case TextureHostFormat::R32G32B32Float: return ScanoutDecode::R32G32B32Float;
    case TextureHostFormat::R32G32B32A32Float: return ScanoutDecode::R32G32B32A32Float;
    default: return std::nullopt;
  }
}

}  // namespace

ScanoutPlan plan_scanout(const PresentationFrame& frame, std::uint32_t target_width,
                         std::uint32_t target_height, bool preserve_aspect_ratio,
                         bool srgb_target) {
  ScanoutPlan plan{};
  if (!frame.texture.valid || !target_width || !target_height) {
    plan.error = "presentation requires a valid texture and non-zero host extent";
    return plan;
  }
  if (frame.texture.dimension != TextureDimension::TwoDOrStacked ||
      frame.texture.stacked || frame.texture.depth != 1) {
    plan.error = "presentation currently requires a non-stacked 2D scanout texture";
    return plan;
  }
  TextureDescriptor descriptor = frame.texture;
  descriptor.mip_min_level = 0;
  descriptor.mip_max_level = 0;
  descriptor.packed_mips = false;
  const auto layout = build_texture_layout(descriptor);
  if (!layout.valid || layout.subresources.empty()) {
    plan.error = layout.error.empty() ? "scanout texture layout is invalid" : layout.error;
    return plan;
  }
  const auto& base = layout.subresources.front();
  const auto source_width = std::min(
      frame.visible_width ? frame.visible_width : descriptor.width, base.width_texels);
  const auto source_height = std::min(
      frame.visible_height ? frame.visible_height : descriptor.height, base.height_texels);
  if (!source_width || !source_height ||
      layout.format.storage != TextureStorage::Uncompressed) {
    plan.error = "presentation requires a non-empty uncompressed base texture";
    return plan;
  }
  const auto bytes_per_block = layout.format.bytes_per_block();
  if (!bytes_per_block || bytes_per_block > 16) {
    plan.error = "presentation source pixel size is unsupported";
    return plan;
  }
  const auto decode = scanout_decode(layout.format.host_format);
  if (!decode) {
    plan.error = "presentation source format has no RGBA8 scanout conversion";
    return plan;
  }
  if (base.guest_size_bytes > UINT32_MAX ||
      std::uint64_t{base.guest_address} + base.guest_size_bytes > 0x100000000ull) {
    plan.error = "scanout source range is invalid";
    return plan;
  }

  std::uint32_t draw_width = target_width;
  std::uint32_t draw_height = target_height;
  if (preserve_aspect_ratio) {
    const double scale = std::min(double(target_width) / double(source_width),
                                  double(target_height) / double(source_height));
    draw_width = std::min(
        target_width,
        std::max(1u, static_cast<std::uint32_t>(std::lround(source_width * scale))));
    draw_height = std::min(
        target_height,
        std::max(1u, static_cast<std::uint32_t>(std::lround(source_height * scale))));
  }

  auto& c = plan.constants;
  c.guest_base = base.guest_address;
  c.pitch_blocks = base.storage_pitch_blocks;
  c.bytes_per_block = bytes_per_block;
  c.flags = (descriptor.tiled ? kScanoutTiled : 0u) |
            (std::uint32_t(descriptor.endian) << kScanoutEndianShift) |
            (srgb_target ? kScanoutSrgbTarget : 0u);
  c.decode = static_cast<std::uint32_t>(*decode);
  c.swizzle = descriptor.swizzle;
  c.source_width = source_width;
  c.source_height = source_height;
  c.offset_x = static_cast<std::int32_t>((target_width - draw_width) / 2u);
  c.offset_y = static_cast<std::int32_t>((target_height - draw_height) / 2u);
  c.draw_width = draw_width;
  c.draw_height = draw_height;
  c.target_width = target_width;
  c.target_height = target_height;
  plan.guest_address = base.guest_address;
  plan.guest_size = static_cast<std::uint32_t>(base.guest_size_bytes);
  plan.valid = true;
  return plan;
}

ScanoutTaps scanout_taps(std::int32_t target, std::int32_t offset,
                         std::uint32_t draw_extent,
                         std::uint32_t source_extent) noexcept {
  // Same arithmetic, in the same order, as taps() in make_scanout_shader().
  float s = (float(target - offset) + 0.5f) * float(source_extent) / float(draw_extent) - 0.5f;
  s = std::clamp(s, 0.0f, float(source_extent - 1u));
  const float base = std::floor(s);
  const float t = s - base;
  ScanoutTaps taps{};
  taps.first = static_cast<std::int32_t>(base) - 1;
  taps.weights[0] = t * (-0.5f + t * (1.0f - 0.5f * t));
  taps.weights[1] = 1.0f + t * t * (-2.5f + 1.5f * t);
  taps.weights[2] = t * (0.5f + t * (2.0f - 1.5f * t));
  taps.weights[3] = t * t * (-0.5f + 0.5f * t);
  return taps;
}

LoweredShader make_scanout_shader() {
  LoweredShader result{};
  result.stage = ShaderStage::Pixel;
  result.entry_point = "main";
  result.profile = "ps_6_0";
  result.hlsl = R"hlsl(struct ScanoutConstants {
  uint guest_base; uint pitch_blocks; uint bytes_per_block; uint flags;
  uint decode; uint swizzle; uint source_width; uint source_height;
  int offset_x; int offset_y; uint draw_width; uint draw_height;
  uint target_width; uint target_height; uint reserved0; uint reserved1;
};
#ifdef __spirv__
[[vk::push_constant]] ScanoutConstants scanout;
#else
ConstantBuffer<ScanoutConstants> scanout : register(b0);
#endif
ByteAddressBuffer guest : register(t0);

uint swap32(uint v, uint endian) {
  if (endian == 1u) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
  if (endian == 2u) return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
  if (endian == 3u) return (v << 16) | (v >> 16);
  return v;
}

// Byte offset of texel (x, y) in the base level: Xenos 2D tiling, or rows.
uint texel_offset(uint x, uint y) {
  const uint bpb = scanout.bytes_per_block;
  if ((scanout.flags & 1u) == 0u) return (y * scanout.pitch_blocks + x) * bpb;
  if ((bpb & (bpb - 1u)) != 0u) return 0u;
  const uint outer = ((y >> 5) * (scanout.pitch_blocks >> 5) + (x >> 5)) << 6;
  const uint inner = (((y >> 1) & 7u) << 3) | (x & 7u);
  const uint bank = (y >> 4) & 1u;
  const uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
  const uint oi = (outer | inner) << firstbitlow(bpb);
  return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) |
         (((oi >> 4) & 1u) << 5) | (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}

uint load_word(uint address) { return guest.Load(address & ~3u); }

float finite01(float v) { return (isnan(v) || isinf(v)) ? 0.0 : saturate(v); }
float half01(uint bits) { return finite01(f16tof32(bits & 0xFFFFu)); }
float to_unorm(uint value, uint max_value) { return float(value) / float(max_value); }

float4 decode_texel(uint x, uint y) {
  const uint address = scanout.guest_base + texel_offset(x, y);
  const uint endian = (scanout.flags >> 1) & 3u;
  const uint bpb = scanout.bytes_per_block;
  uint w0 = 0u, w1 = 0u, w2 = 0u, w3 = 0u;
  if (bpb >= 4u) {
    w0 = swap32(load_word(address), endian);
    if (bpb >= 8u) w1 = swap32(load_word(address + 4u), endian);
    if (bpb >= 12u) w2 = swap32(load_word(address + 8u), endian);
    if (bpb >= 16u) w3 = swap32(load_word(address + 12u), endian);
  } else if (bpb == 2u) {
    w0 = (load_word(address) >> ((address & 2u) * 8u)) & 0xFFFFu;
    if (endian == 1u) w0 = ((w0 & 0xFFu) << 8) | (w0 >> 8);
  } else {
    w0 = (load_word(address) >> ((address & 3u) * 8u)) & 0xFFu;
  }
  const uint v = w0;
  switch (scanout.decode) {
    case 0u: return float4(to_unorm(v & 0xFFu, 255u), 0.0, 0.0, 1.0);
    case 1u: return float4(to_unorm(v & 0xFFu, 255u), to_unorm((v >> 8) & 0xFFu, 255u), 0.0, 1.0);
    case 2u: return float4(to_unorm(v & 0xFFu, 255u), to_unorm((v >> 8) & 0xFFu, 255u),
                           to_unorm((v >> 16) & 0xFFu, 255u), to_unorm(v >> 24, 255u));
    case 3u: return float4(to_unorm((v >> 10) & 31u, 31u), to_unorm((v >> 5) & 31u, 31u),
                           to_unorm(v & 31u, 31u), (v & 0x8000u) != 0u ? 1.0 : 0.0);
    case 4u: return float4(to_unorm((v >> 11) & 31u, 31u), to_unorm((v >> 5) & 63u, 63u),
                           to_unorm(v & 31u, 31u), 1.0);
    case 5u: return float4(to_unorm((v >> 8) & 15u, 15u), to_unorm((v >> 4) & 15u, 15u),
                           to_unorm(v & 15u, 15u), to_unorm((v >> 12) & 15u, 15u));
    case 6u: return float4(to_unorm(v & 1023u, 1023u), to_unorm((v >> 10) & 1023u, 1023u),
                           to_unorm((v >> 20) & 1023u, 1023u), to_unorm((v >> 30) & 3u, 3u));
    case 7u: return float4(to_unorm(v & 0xFFFFu, 65535u), 0.0, 0.0, 1.0);
    case 8u: return float4(to_unorm(v & 0xFFFFu, 65535u), to_unorm(v >> 16, 65535u), 0.0, 1.0);
    case 9u: return float4(to_unorm(w0 & 0xFFFFu, 65535u), to_unorm(w0 >> 16, 65535u),
                           to_unorm(w1 & 0xFFFFu, 65535u), to_unorm(w1 >> 16, 65535u));
    case 10u: return float4(half01(v), 0.0, 0.0, 1.0);
    case 11u: return float4(half01(v), half01(v >> 16), 0.0, 1.0);
    case 12u: return float4(half01(w0), half01(w0 >> 16), half01(w1), half01(w1 >> 16));
    case 13u: return float4(finite01(asfloat(w0)), 0.0, 0.0, 1.0);
    case 14u: return float4(finite01(asfloat(w0)), finite01(asfloat(w1)), 0.0, 1.0);
    case 15u: return float4(finite01(asfloat(w0)), finite01(asfloat(w1)),
                            finite01(asfloat(w2)), 1.0);
    default: return float4(finite01(asfloat(w0)), finite01(asfloat(w1)),
                           finite01(asfloat(w2)), finite01(asfloat(w3)));
  }
}

// Scan-out reads the front buffer through its fetch constant's swizzle: three
// bits per result component, 0-3 select X/Y/Z/W, 4 is 0 and 5 is 1.
float4 swizzled(float4 c) {
  float s[4] = {c.x, c.y, c.z, c.w};
  float o[4];
  [unroll] for (uint i = 0u; i < 4u; ++i) {
    const uint select = (scanout.swizzle >> (i * 3u)) & 7u;
    o[i] = select < 4u ? s[select] : select == 4u ? 0.0 : select == 5u ? 1.0 : s[i];
  }
  return float4(o[0], o[1], o[2], o[3]);
}

// Catmull-Rom taps for one target coordinate; scanout_taps() on the CPU
// computes the same values.
void taps(int target, int offset, uint draw_extent, uint source_extent,
          out int first, out float4 weights) {
  float s = (float(target - offset) + 0.5) * float(source_extent) / float(draw_extent) - 0.5;
  s = clamp(s, 0.0, float(source_extent - 1u));
  const float base = floor(s);
  const float t = s - base;
  first = int(base) - 1;
  weights = float4(t * (-0.5 + t * (1.0 - 0.5 * t)), 1.0 + t * t * (-2.5 + 1.5 * t),
                   t * (0.5 + t * (2.0 - 1.5 * t)), t * t * (-0.5 + 0.5 * t));
}

float3 srgb_to_linear(float3 c) {
  return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

float4 main(float4 position : SV_Position) : SV_Target0 {
  const int2 p = int2(position.xy);
  if (p.x < scanout.offset_x || p.y < scanout.offset_y ||
      p.x >= scanout.offset_x + int(scanout.draw_width) ||
      p.y >= scanout.offset_y + int(scanout.draw_height))
    return float4(0.0, 0.0, 0.0, 0.0);
  int first_x, first_y;
  float4 weight_x, weight_y;
  taps(p.x, scanout.offset_x, scanout.draw_width, scanout.source_width, first_x, weight_x);
  taps(p.y, scanout.offset_y, scanout.draw_height, scanout.source_height, first_y, weight_y);
  float4 sum = float4(0.0, 0.0, 0.0, 0.0);
  [unroll] for (int j = 0; j < 4; ++j) {
    if (weight_y[j] == 0.0) continue;
    const uint y = uint(clamp(first_y + j, 0, int(scanout.source_height) - 1));
    float4 row = float4(0.0, 0.0, 0.0, 0.0);
    [unroll] for (int i = 0; i < 4; ++i) {
      if (weight_x[i] == 0.0) continue;
      const uint x = uint(clamp(first_x + i, 0, int(scanout.source_width) - 1));
      row += weight_x[i] * swizzled(decode_texel(x, y));
    }
    sum += weight_y[j] * row;
  }
  float4 color = saturate(sum);
  if ((scanout.flags & 8u) != 0u) color.rgb = srgb_to_linear(color.rgb);
  return color;
}
)hlsl";
  result.source_hash = 0x5CA9007ull;
  result.translation_hash = std::hash<std::string>{}(result.hlsl);
  result.complete = true;
  return result;
}

}  // namespace xenon::gpu
