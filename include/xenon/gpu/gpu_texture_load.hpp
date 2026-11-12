#pragma once

#include <cstdint>
#include <vector>

#include "xenon/gpu/shader_translation.hpp"
#include "xenon/gpu/texture.hpp"

namespace xenon::gpu {

// A texture loaded on the GPU: a compute pass reads the guest texels where
// they lie - in the guest-memory mirror, or in a copy of guest memory taken at
// a sync point - and writes them untiled, endian-swapped and converted, row
// by row, into a staging buffer the backend then copies into the host image.
// It produces exactly the bytes decode_texture() does, so the CPU never reads,
// untiles or converts a texture, and a texture a resolve wrote never leaves
// the GPU at all.
//
// One dispatch covers `rows` block rows of `slices` z slices of one
// subresource, from (row_start, z_start). Each thread writes one 32-bit word
// of a destination row; a row is row_dwords words, padded with zeros past
// width_blocks * bytes_per_block. Root/push constants; layout shared with
// make_gpu_texture_load_shader().
struct GpuTextureLoadConstants {
  // Where the subresource's guest_address lies in the bound source buffer,
  // and how many bytes of that buffer the load may read (texels past it read
  // as zero).
  std::uint32_t source_offset{};
  std::uint32_t source_limit{};
  std::uint32_t destination_offset{};
  std::uint32_t destination_row_pitch{};
  std::uint32_t destination_slice_pitch{};
  std::uint32_t row_dwords{};
  std::uint32_t width_blocks{};
  std::uint32_t rows{};
  std::uint32_t slices{};
  std::uint32_t row_start{};
  std::uint32_t z_start{};
  std::uint32_t storage_pitch_blocks{};
  std::uint32_t storage_height_blocks{};
  std::uint32_t packed_x{};
  std::uint32_t packed_y{};
  std::uint32_t packed_z{};
  std::uint32_t bytes_per_block{};       // in guest memory
  std::uint32_t flags{};                 // kGpuTextureLoad* below
  std::uint32_t host_bytes_per_block{};  // in the destination rows
  std::uint32_t reserved{};
};
static_assert(sizeof(GpuTextureLoadConstants) == 80);

inline constexpr std::uint32_t kGpuTextureLoadTiled = 1u << 0u;
inline constexpr std::uint32_t kGpuTextureLoad3D = 1u << 1u;
inline constexpr std::uint32_t kGpuTextureLoadEndianShift = 2u;  // two bits, Endian
inline constexpr std::uint32_t kGpuTextureLoadModeShift = 4u;    // two bits, TextureLoad

// A part of a subresource one dispatch and one copy handle.
struct GpuTextureLoadChunk {
  std::uint32_t row_start{};
  std::uint32_t rows{};
  std::uint32_t z_start{};
  std::uint32_t slices{};
};

// Bytes of one destination row: width_blocks * host_bytes_per_block() rounded up to
// whole words, then to `row_alignment` (a power of two of at least 4).
[[nodiscard]] std::uint32_t gpu_texture_load_row_pitch(const TextureLayout& layout,
                                                       const TextureSubresourceLayout& sub,
                                                       std::uint32_t row_alignment) noexcept;

// The whole subresource when its rows fit in `max_bytes` of staging,
// otherwise slices of at most that many bytes (whole rows of one z slice).
[[nodiscard]] std::vector<GpuTextureLoadChunk> plan_gpu_texture_load_chunks(
    const TextureSubresourceLayout& sub, std::uint32_t row_pitch, std::uint64_t max_bytes);

// The constants loading `chunk` of `sub`. The caller sets source_offset and
// source_limit (where the subresource's guest bytes lie in the bound source)
// and destination_offset.
[[nodiscard]] GpuTextureLoadConstants make_gpu_texture_load_constants(
    const TextureLayout& layout, const TextureSubresourceLayout& sub,
    const GpuTextureLoadChunk& chunk, std::uint32_t row_pitch);

// The load compute shader: source buffer at t0 (binding 0), staging buffer at
// u0 (binding 1), 8x8 threads (row word, row) and one z slice per group z.
[[nodiscard]] LoweredShader make_gpu_texture_load_shader();

}  // namespace xenon::gpu
