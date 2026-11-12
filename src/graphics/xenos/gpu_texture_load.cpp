#include "xenon/gpu/gpu_texture_load.hpp"

#include <algorithm>
#include <functional>
#include <string>

namespace xenon::gpu {

std::uint32_t gpu_texture_load_row_pitch(const TextureLayout& layout,
                                         const TextureSubresourceLayout& sub,
                                         std::uint32_t row_alignment) noexcept {
  const auto row_bytes = sub.width_blocks * layout.format.host_bytes_per_block();
  const auto alignment = (std::max)(row_alignment, 4u);
  return (row_bytes + alignment - 1u) / alignment * alignment;
}

std::vector<GpuTextureLoadChunk> plan_gpu_texture_load_chunks(const TextureSubresourceLayout& sub,
                                                              std::uint32_t row_pitch,
                                                              std::uint64_t max_bytes) {
  std::vector<GpuTextureLoadChunk> chunks;
  const auto depth = (std::max)(sub.depth_texels, 1u);
  const auto rows = (std::max)(sub.height_blocks, 1u);
  if (std::uint64_t(row_pitch) * rows * depth <= max_bytes) {
    chunks.push_back({0u, rows, 0u, depth});
    return chunks;
  }
  // A row is at most 8192 blocks of 16 bytes, so any staging buffer holds one.
  const auto rows_per_chunk = static_cast<std::uint32_t>(
      (std::max<std::uint64_t>)(1u, max_bytes / (std::max)(row_pitch, 1u)));
  for (std::uint32_t z = 0; z < depth; ++z)
    for (std::uint32_t row = 0; row < rows; row += rows_per_chunk)
      chunks.push_back({row, (std::min)(rows_per_chunk, rows - row), z, 1u});
  return chunks;
}

GpuTextureLoadConstants make_gpu_texture_load_constants(const TextureLayout& layout,
                                                        const TextureSubresourceLayout& sub,
                                                        const GpuTextureLoadChunk& chunk,
                                                        std::uint32_t row_pitch) {
  GpuTextureLoadConstants c{};
  const auto bpb = layout.format.bytes_per_block();
  c.destination_row_pitch = row_pitch;
  c.destination_slice_pitch = row_pitch * chunk.rows;
  c.row_dwords = (sub.width_blocks * layout.format.host_bytes_per_block() + 3u) / 4u;
  c.width_blocks = sub.width_blocks;
  c.rows = chunk.rows;
  c.slices = chunk.slices;
  c.row_start = chunk.row_start;
  c.z_start = chunk.z_start;
  c.storage_pitch_blocks = sub.storage_pitch_blocks;
  c.storage_height_blocks = sub.storage_height_blocks;
  c.packed_x = sub.packed_x_blocks;
  c.packed_y = sub.packed_y_blocks;
  c.packed_z = sub.packed_z;
  c.bytes_per_block = bpb;
  c.host_bytes_per_block = layout.format.host_bytes_per_block();
  const auto& descriptor = layout.descriptor;
  if (descriptor.tiled) c.flags |= kGpuTextureLoadTiled;
  if (descriptor.dimension == TextureDimension::ThreeD) c.flags |= kGpuTextureLoad3D;
  c.flags |= (std::uint32_t(descriptor.endian) & 3u) << kGpuTextureLoadEndianShift;
  c.flags |= (std::uint32_t(layout.format.load) & 3u) << kGpuTextureLoadModeShift;
  return c;
}

LoweredShader make_gpu_texture_load_shader() {
  LoweredShader result{};
  result.stage = ShaderStage::Compute;
  result.entry_point = "main";
  result.profile = "cs_6_0";
  result.hlsl = R"hlsl(struct LoadConstants {
  uint source_offset; uint source_limit; uint destination_offset; uint destination_row_pitch;
  uint destination_slice_pitch; uint row_dwords; uint width_blocks; uint rows;
  uint slices; uint row_start; uint z_start; uint storage_pitch_blocks;
  uint storage_height_blocks; uint packed_x; uint packed_y; uint packed_z;
  uint bytes_per_block; uint flags; uint host_bytes_per_block; uint reserved;
};
#ifdef __spirv__
[[vk::push_constant]] LoadConstants c;
[[vk::binding(0, 0)]] ByteAddressBuffer source;
[[vk::binding(1, 0)]] RWByteAddressBuffer destination;
#else
ConstantBuffer<LoadConstants> c : register(b0);
ByteAddressBuffer source : register(t0);
RWByteAddressBuffer destination : register(u0);
#endif

// tiled_offset_2d() / tiled_offset_3d().
uint tiled_combine(uint oi, uint bank, uint pipe, uint y_lsb) {
  return (y_lsb << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
         (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}
uint tiled_2d(uint x, uint y, uint pitch, uint log2_bpb) {
  const uint outer = ((y >> 5) * (pitch >> 5) + (x >> 5)) << 6;
  const uint inner = (((y >> 1) & 7u) << 3) | (x & 7u);
  const uint bank = (y >> 4) & 1u;
  const uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
  return tiled_combine((outer | inner) << log2_bpb, bank, pipe, y & 1u);
}
uint tiled_3d(uint x, uint y, uint z, uint pitch, uint height, uint log2_bpb) {
  const uint outer = ((((z >> 2) * (height >> 4) + (y >> 4)) * (pitch >> 5) + (x >> 5)) << 7);
  const uint inner = ((z & 3u) << 5) | (((y >> 1) & 3u) << 3) | (x & 7u);
  const uint bank = ((y >> 3) ^ (z >> 2)) & 1u;
  const uint pipe = ((x >> 3) & 3u) ^ (bank << 1);
  return tiled_combine((outer | inner) << log2_bpb, bank, pipe, y & 1u);
}

// The guest offset of block (x, y, z) of the subresource, as decode_texture().
uint block_offset(uint x, uint y, uint z) {
  const uint sx = x + c.packed_x, sy = y + c.packed_y, sz = z + c.packed_z;
  const uint bpb = c.bytes_per_block;
  if ((c.flags & 1u) != 0u) {
    // tiled_offset_*() give 0 for a block size that is not a power of two.
    if ((bpb & (bpb - 1u)) != 0u) return 0u;
    const uint log2_bpb = firstbitlow(bpb);
    return (c.flags & 2u) != 0u
               ? tiled_3d(sx, sy, sz, c.storage_pitch_blocks, c.storage_height_blocks, log2_bpb)
               : tiled_2d(sx, sy, c.storage_pitch_blocks, log2_bpb);
  }
  return (sz * c.storage_height_blocks + sy) * c.storage_pitch_blocks * bpb + sx * bpb;
}

uint read_byte(uint offset) {
  const uint a = c.source_offset + offset;
  if (a >= c.source_limit) return 0u;
  return (source.Load(a & ~3u) >> ((a & 3u) * 8u)) & 0xFFu;
}
uint read_word(uint offset) {
  const uint a = c.source_offset + offset;
  if ((a & 3u) == 0u && a + 4u <= c.source_limit) return source.Load(a);
  return read_byte(offset) | (read_byte(offset + 1u) << 8) | (read_byte(offset + 2u) << 16) |
         (read_byte(offset + 3u) << 24);
}

// gpu_swap().
uint swap_word(uint v, uint endian) {
  if (endian == 1u) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
  if (endian == 2u) return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
  if (endian == 3u) return (v << 16) | (v >> 16);
  return v;
}

// unpack_depth_stencil()'s depth.
float unorm24_depth(uint bits) { return float(bits + (bits >> 23u)) * (1.0 / 16777216.0); }
float float20e4_depth(uint value) {
  value &= 0xFFFFFFu;
  if (value == 0u) return 0.0;
  uint mantissa = value & 0xFFFFFu;
  int exponent = int(value >> 20u);
  if (exponent == 0) {
    const uint shift = (31u - firstbithigh(mantissa)) - 11u;
    exponent = 1 - int(shift);
    mantissa = (mantissa << shift) & 0xFFFFFu;
  }
  return asfloat((uint(exponent + 112) << 23u) | (mantissa << 3u));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= c.row_dwords || id.y >= c.rows || id.z >= c.slices) return;
  const uint y = c.row_start + id.y;
  const uint z = c.z_start + id.z;
  const uint bpb = c.bytes_per_block;
  const uint endian = (c.flags >> 2) & 3u;
  const uint load = (c.flags >> 4) & 3u;
  uint value = 0u;
  if ((bpb & 3u) == 0u) {
    // Whole words of a block: endian_copy() swaps each one.
    const uint host_bpb = c.host_bytes_per_block;
    const uint byte = id.x * 4u;
    const uint x = byte / host_bpb;
    const uint within = byte - x * host_bpb;
    if (x < c.width_blocks) {
      if (within >= bpb) {
        // Past the guest texel of a widened 32_32_32_FLOAT one: w = 1.0.
        value = 0x3F800000u;
      } else {
        value = swap_word(read_word(block_offset(x, y, z) + within), endian);
        // A packed 24_8 word becomes its depth as float32 (TextureLoad).
        if (load == 1u) value = asuint(unorm24_depth(value >> 8u));
        else if (load == 2u) value = asuint(float20e4_depth(value >> 8u));
      }
    }
  } else {
    // 1- and 2-byte blocks: only a 2-byte block under 8in16 is swapped.
    [unroll] for (uint k = 0u; k < 4u; ++k) {
      const uint byte = id.x * 4u + k;
      const uint x = byte / bpb;
      if (x >= c.width_blocks) break;
      uint j = byte - x * bpb;
      if (bpb == 2u && endian == 1u) j ^= 1u;
      value |= read_byte(block_offset(x, y, z) + j) << (k * 8u);
    }
  }
  destination.Store(c.destination_offset + id.z * c.destination_slice_pitch +
                        id.y * c.destination_row_pitch + id.x * 4u,
                    value);
}
)hlsl";
  result.source_hash = 0x7E10ADull;
  result.translation_hash = std::hash<std::string>{}(result.hlsl);
  result.complete = true;
  return result;
}

}  // namespace xenon::gpu
