#include "xenon/gpu/gpu_resolve.hpp"

#include <bit>
#include <functional>
#include <optional>
#include <sstream>
#include <string>

#include "xenon/gpu/texture.hpp"
#include "xenon/memory/types.hpp"

namespace xenon::gpu {
namespace {

std::optional<GpuResolveSource> resolve_source(ColorRenderTargetFormat format) noexcept {
  switch (color_host_storage(format)) {
    case ColorHostStorage::R8G8B8A8Unorm: return GpuResolveSource::R8G8B8A8Unorm;
    case ColorHostStorage::R10G10B10A2Unorm: return GpuResolveSource::R10G10B10A2Unorm;
    case ColorHostStorage::R16G16Float: return GpuResolveSource::R16G16Float;
    case ColorHostStorage::R16G16B16A16Float: return GpuResolveSource::R16G16B16A16Float;
    case ColorHostStorage::R32Float: return GpuResolveSource::R32Float;
    case ColorHostStorage::R32G32Float: return GpuResolveSource::R32G32Float;
    default: return std::nullopt;
  }
}

// The destination codecs write_converted_resolve() has that the shader
// reproduces bit for bit.
std::optional<GpuResolvePack> convert_pack(std::uint8_t destination_format) noexcept {
  switch (destination_format) {
    case 6: case 14: case 50: return GpuResolvePack::R8G8B8A8;
    case 7: case 54: return GpuResolvePack::R10G10B10A2;
    case 25: return GpuResolvePack::R16G16;
    case 26: return GpuResolvePack::R16G16B16A16;
    case 36: return GpuResolvePack::R32Float;
    case 37: return GpuResolvePack::R32G32Float;
    case 38: return GpuResolvePack::R32G32B32A32Float;
    default: return std::nullopt;
  }
}

GpuResolvePlan invalid(std::string reason) {
  GpuResolvePlan plan{};
  plan.error = std::move(reason);
  return plan;
}

}  // namespace

GpuResolvePlan plan_gpu_color_resolve(const ResolvePlan& plan, ColorRenderTargetFormat format,
                                      const EdramSurfaceLayout& resolve_surface,
                                      const EdramSurfaceLayout& owner,
                                      MsaaSamples owner_host_samples,
                                      ColorRenderTargetFormat owner_format) {
  const auto& copy = plan.copy;
  const auto& rectangle = plan.rectangle;
  if (!plan.valid || plan.depth || rectangle.empty() || rectangle.left < 0 || rectangle.top < 0)
    return invalid("not a non-empty color resolve");
  if (copy.command != CopyCommand::Raw && copy.command != CopyCommand::Convert)
    return invalid("resolve command has no GPU form");
  // The owner's EDRAM words must mean what the resolve reads them as.
  if (owner_format != format || owner.is_64bpp != resolve_surface.is_64bpp)
    return invalid("owner image holds the tiles in another format");
  if (copy.destination_array) return invalid("array resolve destinations resolve on the CPU");
  const auto source = resolve_source(format);
  if (!source) return invalid("owner host storage has no GPU resolve read");

  GpuResolvePlan result{};
  auto& c = result.constants;
  const bool convert =
      copy.command == CopyCommand::Convert || color_host_requires_conversion(format);
  if (convert) {
    const auto pack = convert_pack(copy.destination_format);
    if (!pack) return invalid("destination format has no GPU resolve codec");
    c.destination_format = static_cast<std::uint32_t>(*pack);
    c.flags |= kGpuResolveConvert;
    if (copy.destination_red_blue_swap) c.flags |= kGpuResolveRedBlueSwap;
    c.exponent_bias = copy.destination_exponent_bias;
    result.bytes_per_pixel = texture_format_info(copy.destination_format).bytes_per_block();
  } else {
    if (copy.destination_red_blue_swap || copy.destination_exponent_bias)
      return invalid("raw resolve cannot apply component swap or exponent bias");
    result.bytes_per_pixel = color_host_bytes_per_pixel(format);
    if (texture_format_info(copy.destination_format).bytes_per_block() != result.bytes_per_pixel)
      return invalid("raw resolve destination size differs from the source");
    c.destination_format = static_cast<std::uint32_t>(GpuResolvePack::RawHost);
  }
  if (result.bytes_per_pixel != 4u && result.bytes_per_pixel != 8u &&
      result.bytes_per_pixel != 16u)
    return invalid("destination pixel size has no GPU resolve store");
  c.flags |= std::uint32_t(copy.destination_endian) << kGpuResolveEndianShift;

  // Which guest samples of the resolve surface are read, and how they mix.
  const auto all_samples = (1u << (1u << static_cast<unsigned>(plan.samples))) - 1u;
  c.sample_mask = plan.native_color_average ? all_samples : plan.guest_sample_mask;
  const auto selected = std::popcount(c.sample_mask);
  if (selected != 1 && selected != 2 && selected != 4)
    return invalid("resolve selects an unsupported sample set");
  const bool same_surface = owner.base_tile == resolve_surface.base_tile &&
                            owner.pitch_pixels == resolve_surface.pitch_pixels &&
                            owner.msaa == resolve_surface.msaa;
  if (plan.native_color_average && same_surface) c.flags |= kGpuResolveNativeAverage;

  const auto pitch = (std::uint32_t(copy.destination_pitch) + 31u) & ~31u;
  const auto right = std::uint32_t(rectangle.right);
  const auto bottom = std::uint32_t(rectangle.bottom);
  if (!pitch || right > pitch) return invalid("resolve rectangle is wider than its destination");
  c.transfer = make_edram_transfer_constants(owner, owner_host_samples, resolve_surface,
                                             resolve_surface.msaa);
  c.left = std::uint32_t(rectangle.left);
  c.top = std::uint32_t(rectangle.top);
  c.width = right - c.left;
  c.height = bottom - c.top;
  c.destination_base = copy.destination_base;
  c.destination_pitch = pitch;
  c.source_storage = static_cast<std::uint32_t>(*source);

  // Tiled 2D addressing keeps each 32x32 macro tile in whole 4 KiB pages:
  // address bits from 12 up are the macro tile's offset >> 8.
  const std::uint64_t first_tile =
      std::uint64_t(c.top >> 5u) * (pitch >> 5u) + (c.left >> 5u);
  const std::uint64_t last_tile =
      std::uint64_t((bottom - 1u) >> 5u) * (pitch >> 5u) + ((right - 1u) >> 5u);
  const std::uint64_t tile_bytes = 64u * result.bytes_per_pixel;
  const std::uint64_t low = ((first_tile * tile_bytes) >> 8u) << 12u;
  const std::uint64_t high = ((((last_tile + 1u) * tile_bytes - 1u) >> 8u) + 1u) << 12u;
  const std::uint64_t begin = std::uint64_t(copy.destination_base) + low;
  const std::uint64_t end = std::uint64_t(copy.destination_base) + high;
  if (end > memory::kPhysicalMemorySize || begin >= end)
    return invalid("resolve destination is outside physical memory");
  result.range_address = static_cast<std::uint32_t>(begin);
  result.range_size = static_cast<std::uint32_t>(end - begin);
  result.valid = true;
  return result;
}

LoweredShader make_gpu_resolve_shader(MsaaSamples owner_host_samples) {
  const auto count = 1u << static_cast<unsigned>(owner_host_samples);
  LoweredShader result{};
  result.stage = ShaderStage::Compute;
  result.entry_point = "main";
  result.profile = "cs_6_0";
  std::ostringstream out;
  out << R"hlsl(struct ResolveConstants {
  uint4 destination; uint4 source; uint4 extent;
  uint left; uint top; uint width; uint height;
  uint destination_base; uint destination_pitch; uint destination_format; uint source_storage;
  uint sample_mask; uint flags; int exponent_bias; uint reserved;
};
#ifdef __spirv__
[[vk::push_constant]] ResolveConstants c;
[[vk::binding(0, 1)]] RWByteAddressBuffer guest;
#else
ConstantBuffer<ResolveConstants> c : register(b0);
RWByteAddressBuffer guest : register(u0);
#endif
)hlsl";
  if (count == 1)
    out << "Texture2D<float4> Source : register(t0);\n";
  else
    out << "Texture2DMS<float4," << count << "> Source : register(t0);\n";
  out << R"hlsl(
// The owner-image sample holding guest sample `guest` of resolve-surface
// pixel (x, y): the EDRAM transfer mapping, resolve surface to owner.
bool owner_sample(uint x, uint y, uint guest, out int2 position, out uint host) {
  position = int2(0, 0);
  host = 0u;
  const uint dst_base = c.destination.x, dst_pitch = c.destination.y, dst_msaa = c.destination.z;
  const uint src_base = c.source.x, src_pitch = c.source.y;
  const uint src_msaa = c.source.z, src_mode = c.source.w;
  const uint stride = c.extent.x;
  const uint sample_x = (x << (dst_msaa == 2u ? 1u : 0u)) | (dst_msaa == 2u ? (guest & 1u) : 0u);
  const uint sample_y = (y << (dst_msaa == 0u ? 0u : 1u)) |
                        (dst_msaa == 0u ? 0u : (dst_msaa == 1u ? guest : (guest >> 1u)));
  const uint in_tile_x = sample_x % 80u;
  const uint row_in_tile = sample_y % 16u;
  const uint tile = (dst_base + (sample_y / 16u) * dst_pitch + (sample_x / 80u) * stride) % 2048u;
  const uint relative = (tile + 2048u - src_base) % 2048u;
  const uint source_sample_x = ((relative % src_pitch) / stride) * 80u + in_tile_x;
  const uint source_sample_y = (relative / src_pitch) * 16u + row_in_tile;
  const uint source_x = source_sample_x >> (src_msaa == 2u ? 1u : 0u);
  const uint source_y = source_sample_y >> (src_msaa == 0u ? 0u : 1u);
  if (source_x >= c.extent.z || source_y >= c.extent.w) return false;
  uint source_guest = 0u;
  if (src_msaa == 2u) source_guest = (source_sample_y & 1u) * 2u + (source_sample_x & 1u);
  else if (src_msaa == 1u) source_guest = source_sample_y & 1u;
  host = src_mode == 1u ? (source_guest != 0u ? 0u : 1u)
       : (src_mode == 2u ? (source_guest != 0u ? 3u : 0u) : source_guest);
  position = int2(source_x, source_y);
  return true;
}

float4 load_owner(int2 position, uint host) {
)hlsl";
  if (count == 1)
    out << "  return Source.Load(int3(position, 0));\n}\n";
  else
    out << "  return Source.Load(position, host);\n}\n";
  out << R"hlsl(
// A decoded sample as decode_host_color_sample() gives it.
float4 decoded(float4 v) {
  switch (c.source_storage) {
    case 2u: return float4(v.xy, 0.0, 1.0);
    case 4u: return float4(v.x, 0.0, 0.0, 1.0);
    case 5u: return float4(v.xy, 0.0, 1.0);
    default: return v;
  }
}

float unorm_step(float v, float maximum) { return round(saturate(v) * maximum) / maximum; }

// The value re-encoded into the owner's host storage, as averaging does
// between steps.
float4 host_quantized(float4 v) {
  switch (c.source_storage) {
    case 0u: return float4(unorm_step(v.x, 255.0), unorm_step(v.y, 255.0),
                           unorm_step(v.z, 255.0), unorm_step(v.w, 255.0));
    case 1u: return float4(unorm_step(v.x, 1023.0), unorm_step(v.y, 1023.0),
                           unorm_step(v.z, 1023.0), unorm_step(v.w, 3.0));
    case 2u: case 3u: return f16tof32(f32tof16(v));
    default: return v;
  }
}

uint unorm_bits(float v, float maximum) { return uint(round(saturate(v) * maximum)); }

uint swap8in16(uint v) { return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8); }
uint swap8in32(uint v) { return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24); }

// One destination word, with the destination's 128-bit endian swap applied
// as apply_endian128() does to the block it lands in.
void store_word(uint address, uint value) {
  const uint endian = (c.flags >> 2) & 7u;
  const uint block = address & ~15u;
  const uint word = (address >> 2) & 3u;
  if (endian == 1u) guest.Store(address, swap8in16(value));
  else if (endian == 2u) guest.Store(address, swap8in32(value));
  else if (endian == 3u) guest.Store(address, (value << 16) | (value >> 16));
  else if (endian == 4u) guest.Store(block + ((word ^ 1u) << 2), swap8in32(value));
  else if (endian == 5u) guest.Store(block + ((3u - word) << 2), swap8in32(value));
  else guest.Store(address, value);
}

uint tiled_offset(uint x, uint y, uint log2_bpp) {
  const uint outer = ((y >> 5) * (c.destination_pitch >> 5) + (x >> 5)) << 6;
  const uint inner = (((y >> 1) & 7u) << 3) | (x & 7u);
  const uint bank = (y >> 4) & 1u;
  const uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
  const uint oi = (outer | inner) << log2_bpp;
  return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) |
         (((oi >> 4) & 1u) << 5) | (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= c.width || id.y >= c.height) return;
  const uint x = c.left + id.x;
  const uint y = c.top + id.y;

  // Read the selected guest samples; average them as the CPU resolve does.
  float4 samples[4];
  uint count = 0u;
  [unroll] for (uint s = 0u; s < 4u; ++s) {
    if ((c.sample_mask & (1u << s)) == 0u) continue;
    int2 position;
    uint host;
    samples[count] = owner_sample(x, y, s, position, host)
                         ? decoded(load_owner(position, host)) : float4(0.0, 0.0, 0.0, 0.0);
    ++count;
  }
  float4 color = samples[0];
  if (count > 1u) {
    if ((c.flags & 32u) != 0u) {
      // A native average of the whole surface: one rounding at the end.
      float4 sum = samples[0];
      for (uint i = 1u; i < count; ++i) sum += samples[i];
      color = host_quantized(sum / float(count));
    } else {
      color = host_quantized((samples[0] + samples[1]) * 0.5);
      if (count == 4u)
        color = host_quantized((color + host_quantized((samples[2] + samples[3]) * 0.5)) * 0.5);
    }
  }

  uint4 words = uint4(0u, 0u, 0u, 0u);
  uint word_count = 1u;
  if ((c.flags & 1u) == 0u) {
    // Raw: the owner's host storage bits.
    switch (c.source_storage) {
      case 0u: words.x = unorm_bits(color.x, 255.0) | (unorm_bits(color.y, 255.0) << 8) |
                         (unorm_bits(color.z, 255.0) << 16) | (unorm_bits(color.w, 255.0) << 24);
               break;
      case 1u: words.x = unorm_bits(color.x, 1023.0) | (unorm_bits(color.y, 1023.0) << 10) |
                         (unorm_bits(color.z, 1023.0) << 20) | (unorm_bits(color.w, 3.0) << 30);
               break;
      case 2u: words.x = f32tof16(color.x) | (f32tof16(color.y) << 16); break;
      case 3u: words.x = f32tof16(color.x) | (f32tof16(color.y) << 16);
               words.y = f32tof16(color.z) | (f32tof16(color.w) << 16);
               word_count = 2u;
               break;
      case 4u: words.x = asuint(color.x); break;
      default: words.x = asuint(color.x); words.y = asuint(color.y); word_count = 2u; break;
    }
  } else {
    float4 v = ldexp(color, float4(c.exponent_bias, c.exponent_bias, c.exponent_bias,
                                   c.exponent_bias));
    if ((c.flags & 2u) != 0u) v = v.zyxw;
    switch (c.destination_format) {
      case 1u: words.x = unorm_bits(v.x, 255.0) | (unorm_bits(v.y, 255.0) << 8) |
                         (unorm_bits(v.z, 255.0) << 16) | (unorm_bits(v.w, 255.0) << 24);
               break;
      case 2u: words.x = unorm_bits(v.x, 1023.0) | (unorm_bits(v.y, 1023.0) << 10) |
                         (unorm_bits(v.z, 1023.0) << 20) | (unorm_bits(v.w, 3.0) << 30);
               break;
      case 3u: words.x = unorm_bits(v.x, 65535.0) | (unorm_bits(v.y, 65535.0) << 16); break;
      case 4u: words.x = unorm_bits(v.x, 65535.0) | (unorm_bits(v.y, 65535.0) << 16);
               words.y = unorm_bits(v.z, 65535.0) | (unorm_bits(v.w, 65535.0) << 16);
               word_count = 2u;
               break;
      case 5u: words.x = asuint(v.x); break;
      case 6u: words.x = asuint(v.x); words.y = asuint(v.y); word_count = 2u; break;
      default: words = asuint(v); word_count = 4u; break;
    }
  }
  const uint log2_bpp = word_count == 1u ? 2u : (word_count == 2u ? 3u : 4u);
  const uint address = c.destination_base + tiled_offset(x, y, log2_bpp);
  store_word(address, words.x);
  if (word_count > 1u) store_word(address + 4u, words.y);
  if (word_count > 2u) {
    store_word(address + 8u, words.z);
    store_word(address + 12u, words.w);
  }
}
)hlsl";
  result.hlsl = out.str();
  result.source_hash = 0x6E50171Eull ^ count;
  result.translation_hash = std::hash<std::string>{}(result.hlsl);
  result.complete = count == 1 || count == 2 || count == 4;
  if (!result.complete) result.diagnostics.emplace_back("unsupported resolve owner sample count");
  return result;
}


bool plan_gpu_color_clear(const ResolvePlan& plan, ColorRenderTargetFormat format,
                          const EdramSurfaceLayout& resolve_surface,
                          const EdramSurfaceLayout& owner, MsaaSamples owner_host_samples,
                          GpuClearConstants& constants) {
  constants = {};
  if (!plan.valid || plan.depth || plan.rectangle.empty() || !resolve_source(format))
    return false;
  constants.transfer = make_edram_transfer_constants(resolve_surface, resolve_surface.msaa,
                                                     owner, owner_host_samples);
  constants.left = std::uint32_t(plan.rectangle.left);
  constants.top = std::uint32_t(plan.rectangle.top);
  constants.right = std::uint32_t(plan.rectangle.right);
  constants.bottom = std::uint32_t(plan.rectangle.bottom);
  const auto sample = unpack_color_sample(format, plan.copy.color_clear);
  for (unsigned i = 0; i < 4; ++i) constants.color[i] = sample.components[i];
  return true;
}

LoweredShader make_edram_clear_shader() {
  LoweredShader result{};
  result.stage = ShaderStage::Pixel;
  result.entry_point = "main";
  result.profile = "ps_6_0";
  result.hlsl = R"hlsl(struct ClearConstants {
  uint4 destination; uint4 source; uint4 extent;
  uint left; uint top; uint right; uint bottom;
  float4 color;
};
#ifdef __spirv__
[[vk::push_constant]] ClearConstants c;
#else
ConstantBuffer<ClearConstants> c : register(b0);
#endif
float4 main(float4 position : SV_Position, uint host_sample : SV_SampleIndex) : SV_Target0 {
  const uint x = uint(position.x);
  const uint y = uint(position.y);
  const uint dst_base = c.destination.x, dst_pitch = c.destination.y;
  const uint dst_msaa = c.destination.z, dst_mode = c.destination.w;
  const uint src_base = c.source.x, src_pitch = c.source.y, src_msaa = c.source.z;
  const uint stride = c.extent.x;
  // This owner host sample's guest sample.
  uint guest = host_sample;
  if (dst_mode == 1u) {
    guest = host_sample != 0u ? 0u : 1u;
  } else if (dst_mode == 2u) {
    if (host_sample == 0u) guest = 0u;
    else if (host_sample == 3u) guest = 1u;
    else discard;
  }
  // Its EDRAM word, as a pixel of the resolve surface.
  const uint sample_x = (x << (dst_msaa == 2u ? 1u : 0u)) | (dst_msaa == 2u ? (guest & 1u) : 0u);
  const uint sample_y = (y << (dst_msaa == 0u ? 0u : 1u)) |
                        (dst_msaa == 0u ? 0u : (dst_msaa == 1u ? guest : (guest >> 1u)));
  const uint in_tile_x = sample_x % 80u;
  const uint row_in_tile = sample_y % 16u;
  const uint tile = (dst_base + (sample_y / 16u) * dst_pitch + (sample_x / 80u) * stride) % 2048u;
  const uint relative = (tile + 2048u - src_base) % 2048u;
  const uint surface_sample_x = ((relative % src_pitch) / stride) * 80u + in_tile_x;
  const uint surface_sample_y = (relative / src_pitch) * 16u + row_in_tile;
  const uint surface_x = surface_sample_x >> (src_msaa == 2u ? 1u : 0u);
  const uint surface_y = surface_sample_y >> (src_msaa == 0u ? 0u : 1u);
  if (surface_x < c.left || surface_x >= c.right || surface_y < c.top || surface_y >= c.bottom)
    discard;
  return c.color;
}
)hlsl";
  result.source_hash = 0xC1EA2ull;
  result.translation_hash = std::hash<std::string>{}(result.hlsl);
  result.complete = true;
  return result;
}
}  // namespace xenon::gpu
