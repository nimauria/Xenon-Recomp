#include "xenon/gpu/texture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>

namespace xenon::gpu {
namespace {

constexpr std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
  return (value + alignment - 1u) / alignment * alignment;
}

constexpr std::uint32_t ceil_div(std::uint32_t value, std::uint32_t divisor) {
  return (value + divisor - 1u) / divisor;
}

constexpr TextureFormatInfo f(std::uint8_t id, std::string_view name,
                              TextureStorage storage, std::uint8_t bw,
                              std::uint8_t bh, std::uint8_t bpp,
                              TextureHostFormat host) {
  return {id, name, storage, bw, bh, bpp, host};
}

constexpr std::array<TextureFormatInfo, 64> kFormats{{
    f(0,"1_REVERSE",TextureStorage::Uncompressed,1,1,1,TextureHostFormat::Unsupported),
    f(1,"1",TextureStorage::Uncompressed,1,1,1,TextureHostFormat::Unsupported),
    f(2,"8",TextureStorage::Uncompressed,1,1,8,TextureHostFormat::R8Unorm),
    f(3,"1_5_5_5",TextureStorage::Uncompressed,1,1,16,TextureHostFormat::B5G5R5A1Unorm),
    f(4,"5_6_5",TextureStorage::Uncompressed,1,1,16,TextureHostFormat::B5G6R5Unorm),
    f(5,"6_5_5",TextureStorage::Uncompressed,1,1,16,TextureHostFormat::Unsupported),
    f(6,"8_8_8_8",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R8G8B8A8Unorm),
    f(7,"2_10_10_10",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R10G10B10A2Unorm),
    f(8,"8_A",TextureStorage::Uncompressed,1,1,8,TextureHostFormat::R8Unorm),
    f(9,"8_B",TextureStorage::Uncompressed,1,1,8,TextureHostFormat::R8Unorm),
    f(10,"8_8",TextureStorage::Uncompressed,1,1,16,TextureHostFormat::R8G8Unorm),
    f(11,"Cr_Y1_Cb_Y0_REP",TextureStorage::Packed,2,1,16,TextureHostFormat::Unsupported),
    f(12,"Y1_Cr_Y0_Cb_REP",TextureStorage::Packed,2,1,16,TextureHostFormat::Unsupported),
    f(13,"16_16_EDRAM",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R16G16Unorm),
    f(14,"8_8_8_8_A",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R8G8B8A8Unorm),
    f(15,"4_4_4_4",TextureStorage::Uncompressed,1,1,16,TextureHostFormat::B4G4R4A4Unorm),
    f(16,"10_11_11",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R11G11B10Float),
    f(17,"11_11_10",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R11G11B10Float),
    f(18,"DXT1",TextureStorage::BlockCompressed,4,4,4,TextureHostFormat::BC1Unorm),
    f(19,"DXT2_3",TextureStorage::BlockCompressed,4,4,8,TextureHostFormat::BC2Unorm),
    f(20,"DXT4_5",TextureStorage::BlockCompressed,4,4,8,TextureHostFormat::BC3Unorm),
    f(21,"16_16_16_16_EDRAM",TextureStorage::Uncompressed,1,1,64,TextureHostFormat::R16G16B16A16Unorm),
    // Xenos depth values need explicit depth/stencil conversion. GPU 10 owns
    // that EDRAM/depth path, so do not claim a lossless sampled-image mapping.
    f(22,"24_8",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::Unsupported),
    f(23,"24_8_FLOAT",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::Unsupported),
    f(24,"16",TextureStorage::Uncompressed,1,1,16,TextureHostFormat::R16Unorm),
    f(25,"16_16",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R16G16Unorm),
    f(26,"16_16_16_16",TextureStorage::Uncompressed,1,1,64,TextureHostFormat::R16G16B16A16Unorm),
    f(27,"16_EXPAND",TextureStorage::Uncompressed,1,1,16,TextureHostFormat::Unsupported),
    f(28,"16_16_EXPAND",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::Unsupported),
    f(29,"16_16_16_16_EXPAND",TextureStorage::Uncompressed,1,1,64,TextureHostFormat::Unsupported),
    f(30,"16_FLOAT",TextureStorage::Uncompressed,1,1,16,TextureHostFormat::R16Float),
    f(31,"16_16_FLOAT",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R16G16Float),
    f(32,"16_16_16_16_FLOAT",TextureStorage::Uncompressed,1,1,64,TextureHostFormat::R16G16B16A16Float),
    f(33,"32",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R32Uint),
    f(34,"32_32",TextureStorage::Uncompressed,1,1,64,TextureHostFormat::R32G32Uint),
    f(35,"32_32_32_32",TextureStorage::Uncompressed,1,1,128,TextureHostFormat::R32G32B32A32Uint),
    f(36,"32_FLOAT",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R32Float),
    f(37,"32_32_FLOAT",TextureStorage::Uncompressed,1,1,64,TextureHostFormat::R32G32Float),
    f(38,"32_32_32_32_FLOAT",TextureStorage::Uncompressed,1,1,128,TextureHostFormat::R32G32B32A32Float),
    f(39,"32_AS_8",TextureStorage::Packed,4,1,8,TextureHostFormat::Unsupported),
    f(40,"32_AS_8_8",TextureStorage::Packed,2,1,16,TextureHostFormat::Unsupported),
    f(41,"16_MPEG",TextureStorage::Uncompressed,1,1,16,TextureHostFormat::Unsupported),
    f(42,"16_16_MPEG",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::Unsupported),
    f(43,"8_INTERLACED",TextureStorage::Uncompressed,1,1,8,TextureHostFormat::Unsupported),
    f(44,"32_AS_8_INTERLACED",TextureStorage::Packed,4,1,8,TextureHostFormat::Unsupported),
    f(45,"32_AS_8_8_INTERLACED",TextureStorage::Packed,1,1,16,TextureHostFormat::Unsupported),
    f(46,"16_INTERLACED",TextureStorage::Uncompressed,1,1,16,TextureHostFormat::Unsupported),
    f(47,"16_MPEG_INTERLACED",TextureStorage::Uncompressed,1,1,16,TextureHostFormat::Unsupported),
    f(48,"16_16_MPEG_INTERLACED",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::Unsupported),
    f(49,"DXN",TextureStorage::BlockCompressed,4,4,8,TextureHostFormat::BC5Unorm),
    f(50,"8_8_8_8_AS_16_16_16_16",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R8G8B8A8Unorm),
    f(51,"DXT1_AS_16_16_16_16",TextureStorage::BlockCompressed,4,4,4,TextureHostFormat::BC1Unorm),
    f(52,"DXT2_3_AS_16_16_16_16",TextureStorage::BlockCompressed,4,4,8,TextureHostFormat::BC2Unorm),
    f(53,"DXT4_5_AS_16_16_16_16",TextureStorage::BlockCompressed,4,4,8,TextureHostFormat::BC3Unorm),
    f(54,"2_10_10_10_AS_16_16_16_16",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R10G10B10A2Unorm),
    f(55,"10_11_11_AS_16_16_16_16",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R11G11B10Float),
    f(56,"11_11_10_AS_16_16_16_16",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R11G11B10Float),
    f(57,"32_32_32_FLOAT",TextureStorage::Uncompressed,1,1,96,TextureHostFormat::R32G32B32Float),
    f(58,"DXT3A",TextureStorage::BlockCompressed,4,4,4,TextureHostFormat::BC4Unorm),
    f(59,"DXT5A",TextureStorage::BlockCompressed,4,4,4,TextureHostFormat::BC4Unorm),
    f(60,"CTX1",TextureStorage::BlockCompressed,4,4,4,TextureHostFormat::Unsupported),
    f(61,"DXT3A_AS_1_1_1_1",TextureStorage::BlockCompressed,4,4,4,TextureHostFormat::BC4Unorm),
    f(62,"8_8_8_8_GAMMA_EDRAM",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::R8G8B8A8Srgb),
    f(63,"2_10_10_10_FLOAT_EDRAM",TextureStorage::Uncompressed,1,1,32,TextureHostFormat::Unsupported),
}};

std::uint64_t tiled_combine(std::uint64_t outer_inner, std::uint32_t bank,
                            std::uint32_t pipe, std::uint32_t y_lsb) noexcept {
  return (std::uint64_t(y_lsb) << 4) | (std::uint64_t(pipe) << 6) |
         (std::uint64_t(bank) << 11) | (outer_inner & 0xFu) |
         (((outer_inner >> 4) & 1u) << 5) |
         (((outer_inner >> 5) & 7u) << 8) | ((outer_inner >> 8) << 12);
}

void endian_copy(std::byte* destination, const std::byte* source,
                 std::uint32_t size, Endian endian) {
  std::uint32_t offset = 0;
  for (; offset + 4 <= size; offset += 4) {
    std::uint32_t value{};
    std::memcpy(&value, source + offset, 4);
    value = gpu_swap(value, endian);
    std::memcpy(destination + offset, &value, 4);
  }
  if (offset < size) {
    std::memcpy(destination + offset, source + offset, size - offset);
    if (endian == Endian::Swap8In16 && size - offset == 2)
      std::swap(destination[offset], destination[offset + 1]);
  }
}

std::uint32_t packed_level(const TextureDescriptor& descriptor) {
  const auto shortest = std::max(1u, std::min(descriptor.width, descriptor.height));
  const auto log2 = std::bit_width(shortest - 1u);
  return log2 > 4 ? log2 - 4 : 0;
}

void packed_origin(const TextureDescriptor& descriptor, const TextureFormatInfo& format,
                   std::uint32_t mip, std::uint32_t& x, std::uint32_t& y,
                   std::uint32_t& z) {
  x = y = z = 0;
  const auto width_log2 = std::bit_width(descriptor.width - 1u);
  const auto height_log2 = std::bit_width(descriptor.height - 1u);
  const auto base = packed_level(descriptor);
  if (mip < base) return;
  const auto packed = mip - base;
  if (packed < 3) {
    if (width_log2 > height_log2) y = 16u >> packed;
    else x = 16u >> packed;
  } else {
    std::uint32_t offset{};
    if (width_log2 > height_log2) {
      offset = (1u << (width_log2 - base)) >> (packed - 2u);
      x = offset;
    } else {
      offset = (1u << (height_log2 - base)) >> (packed - 2u);
      y = offset;
    }
    if (offset < 4) {
      const auto depth_log2 = static_cast<std::uint32_t>(
          std::bit_width(descriptor.depth - 1u));
      z = depth_log2 > 1u + packed ? (depth_log2 - packed) * 4u : 4u;
    }
  }
  x /= format.block_width;
  y /= format.block_height;
}

}  // namespace

const TextureFormatInfo& texture_format_info(std::uint8_t format) noexcept {
  return kFormats[format & 63u];
}

std::uint64_t tiled_offset_2d(std::uint32_t x, std::uint32_t y,
                              std::uint32_t pitch,
                              std::uint32_t bytes_per_block) noexcept {
  if (!bytes_per_block || !std::has_single_bit(bytes_per_block)) return 0;
  const auto log2_bpb = std::countr_zero(bytes_per_block);
  const std::uint64_t outer = (((y >> 5) * (pitch >> 5) + (x >> 5)) << 6);
  const std::uint64_t inner = (((y >> 1) & 7u) << 3) | (x & 7u);
  const auto bank = (y >> 4) & 1u;
  const auto pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
  return tiled_combine((outer | inner) << log2_bpb, bank, pipe, y & 1u);
}

std::uint64_t tiled_offset_3d(std::uint32_t x, std::uint32_t y, std::uint32_t z,
                              std::uint32_t pitch, std::uint32_t height,
                              std::uint32_t bytes_per_block) noexcept {
  if (!bytes_per_block || !std::has_single_bit(bytes_per_block)) return 0;
  const auto log2_bpb = std::countr_zero(bytes_per_block);
  const std::uint64_t outer =
      ((((z >> 2) * (height >> 4) + (y >> 4)) * (pitch >> 5) + (x >> 5)) << 7);
  const std::uint64_t inner = ((z & 3u) << 5) | (((y >> 1) & 3u) << 3) | (x & 7u);
  const auto bank = ((y >> 3) ^ (z >> 2)) & 1u;
  const auto pipe = ((x >> 3) & 3u) ^ (bank << 1);
  return tiled_combine((outer | inner) << log2_bpb, bank, pipe, y & 1u);
}

TextureLayout build_texture_layout(const TextureDescriptor& descriptor) {
  TextureLayout result{};
  result.descriptor = descriptor;
  result.format = texture_format_info(descriptor.format);
  if (!descriptor.valid || !descriptor.width || !descriptor.height ||
      !descriptor.depth || !result.format.bytes_per_block()) {
    result.error = "invalid Xenos texture descriptor";
    return result;
  }
  const auto max_dimension = std::max({descriptor.width, descriptor.height,
                                       descriptor.depth});
  const auto maximum_mip = std::bit_width(max_dimension) - 1u;
  const auto first_mip = std::min<std::uint32_t>(descriptor.mip_min_level, maximum_mip);
  const auto last_mip = std::min<std::uint32_t>(descriptor.mip_max_level, maximum_mip);
  const auto array_layers = descriptor.dimension == TextureDimension::Cube
                                ? 6u
                                : descriptor.dimension == TextureDimension::TwoDOrStacked && descriptor.stacked
                                      ? descriptor.depth
                                      : 1u;
  const auto tail_level = descriptor.packed_mips ? packed_level(descriptor)
                                                  : std::numeric_limits<std::uint32_t>::max();
  std::uint64_t mip_cursor = 0;
  std::uint64_t packed_tail_offset = 0;
  std::uint64_t linear_cursor = 0;
  for (std::uint32_t mip = first_mip; mip <= last_mip; ++mip) {
    const auto width = std::max(1u, descriptor.width >> mip);
    const auto height = descriptor.dimension == TextureDimension::OneD
                            ? 1u : std::max(1u, descriptor.height >> mip);
    const auto depth = descriptor.dimension == TextureDimension::ThreeD
                           ? std::max(1u, descriptor.depth >> mip) : 1u;
    const auto width_blocks = ceil_div(width, result.format.block_width);
    const auto height_blocks = ceil_div(height, result.format.block_height);
    const bool packed = descriptor.packed_mips && mip >= tail_level;
    const auto storage_mip = packed ? tail_level : mip;
    const auto pitch_texels = mip == 0 && descriptor.pitch
                                  ? descriptor.pitch
                                  : std::max(1u, std::bit_ceil(descriptor.width) >> storage_mip);
    const auto pitch_blocks = align_up(ceil_div(pitch_texels, result.format.block_width), 32);
    const auto storage_height = descriptor.dimension == TextureDimension::OneD
                                    ? 1u : align_up(ceil_div(
                                          mip == 0 ? descriptor.height
                                                   : std::max(1u, std::bit_ceil(descriptor.height) >> storage_mip),
                                          result.format.block_height), 32);
    auto row_pitch = pitch_blocks * result.format.bytes_per_block();
    if (!descriptor.tiled && mip != 0) row_pitch = align_up(row_pitch, 256);
    const auto slice_stride = std::uint64_t(row_pitch) * storage_height;
    const auto layer_stride = align_up(static_cast<std::uint32_t>(
        std::min<std::uint64_t>(slice_stride * align_up(depth, 4),
                                std::numeric_limits<std::uint32_t>::max())), 4096);
    std::uint32_t packed_x{}, packed_y{}, packed_z{};
    if (packed) packed_origin(descriptor, result.format, mip, packed_x, packed_y, packed_z);
    if (mip != 0 && packed && (mip == tail_level || tail_level == 0))
      packed_tail_offset = mip_cursor;
    const auto level_offset = mip == 0 ? 0u :
                              packed ? packed_tail_offset : mip_cursor;
    for (std::uint32_t layer = 0; layer < array_layers; ++layer) {
      TextureSubresourceLayout sub{};
      sub.mip_level = mip;
      sub.array_layer = layer;
      sub.guest_address = (mip == 0 ? descriptor.base_address :
                           (descriptor.mip_address ? descriptor.mip_address
                                                   : descriptor.base_address)) +
                          static_cast<std::uint32_t>(level_offset +
                                                     std::uint64_t(layer) * layer_stride);
      sub.width_texels = width;
      sub.height_texels = height;
      sub.depth_texels = depth;
      sub.width_blocks = width_blocks;
      sub.height_blocks = height_blocks;
      sub.storage_pitch_blocks = pitch_blocks;
      sub.storage_height_blocks = storage_height;
      sub.packed_x_blocks = packed_x;
      sub.packed_y_blocks = packed_y;
      sub.packed_z = packed_z;
      sub.guest_size_bytes = layer_stride;
      sub.linear_offset_bytes = linear_cursor;
      sub.linear_row_pitch_bytes = width_blocks * result.format.bytes_per_block();
      linear_cursor += std::uint64_t(sub.linear_row_pitch_bytes) * height_blocks * depth;
      result.subresources.push_back(sub);
    }
    if (mip == 0) result.base_extent_bytes = layer_stride * array_layers;
    else if (!packed || mip == tail_level || tail_level == 0) {
      mip_cursor += layer_stride * array_layers;
      result.mip_extent_bytes = std::max(result.mip_extent_bytes, mip_cursor);
    }
    if (mip == last_mip) break;
  }
  result.linear_size_bytes = linear_cursor;
  result.valid = true;
  return result;
}

DecodedTexture decode_texture(const TextureDescriptor& descriptor,
                              std::span<const std::byte> physical_memory) {
  DecodedTexture result{};
  result.layout = build_texture_layout(descriptor);
  if (!result.layout.valid || !result.layout.format.host_supported()) {
    result.error = result.layout.valid ? "Xenos texture format has no lossless native mapping"
                                       : result.layout.error;
    return result;
  }
  result.linear_data.resize(static_cast<std::size_t>(result.layout.linear_size_bytes));
  const auto bpb = result.layout.format.bytes_per_block();
  for (const auto& sub : result.layout.subresources) {
    for (std::uint32_t z = 0; z < sub.depth_texels; ++z) {
      for (std::uint32_t y = 0; y < sub.height_blocks; ++y) {
        for (std::uint32_t x = 0; x < sub.width_blocks; ++x) {
          const auto sx = x + sub.packed_x_blocks;
          const auto sy = y + sub.packed_y_blocks;
          const auto sz = z + sub.packed_z;
          std::uint64_t source_offset{};
          if (descriptor.tiled) {
            source_offset = descriptor.dimension == TextureDimension::ThreeD
                ? tiled_offset_3d(sx, sy, sz, sub.storage_pitch_blocks,
                                  sub.storage_height_blocks, bpb)
                : tiled_offset_2d(sx, sy, sub.storage_pitch_blocks, bpb);
          } else {
            source_offset = std::uint64_t(sz) * sub.storage_height_blocks *
                                sub.storage_pitch_blocks * bpb +
                            std::uint64_t(sy) * sub.storage_pitch_blocks * bpb +
                            std::uint64_t(sx) * bpb;
          }
          const auto source = std::uint64_t(sub.guest_address) + source_offset;
          const auto destination = sub.linear_offset_bytes +
              (std::uint64_t(z) * sub.height_blocks + y) * sub.linear_row_pitch_bytes +
              std::uint64_t(x) * bpb;
          if (source + bpb > physical_memory.size() ||
              destination + bpb > result.linear_data.size()) {
            result.error = "Xenos texture references guest memory outside physical RAM";
            result.linear_data.clear();
            return result;
          }
          endian_copy(result.linear_data.data() + destination,
                      physical_memory.data() + source, bpb, descriptor.endian);
        }
      }
    }
  }
  result.valid = true;
  return result;
}

void TextureDirtyTracker::track(std::uint64_t key, const TextureLayout& layout) {
  const std::scoped_lock lock(mutex_);
  Entry entry{};
  for (const auto& sub : layout.subresources) {
    entry.ranges.emplace_back(sub.guest_address,
                              std::uint64_t(sub.guest_address) + sub.guest_size_bytes);
  }
  entries_[key] = std::move(entry);
}
void TextureDirtyTracker::erase(std::uint64_t key) {
  const std::scoped_lock lock(mutex_); entries_.erase(key);
}
void TextureDirtyTracker::clear() {
  const std::scoped_lock lock(mutex_); entries_.clear();
}
void TextureDirtyTracker::mark_dirty(std::uint32_t address, std::uint32_t size) noexcept {
  if (!size) return;
  const std::scoped_lock lock(mutex_);
  const auto begin = std::uint64_t(address);
  const auto end = begin + size;
  for (auto& [key, entry] : entries_) {
    (void)key;
    for (const auto& range : entry.ranges) {
      if (begin < range.second && end > range.first) {
        entry.dirty = true;
        break;
      }
    }
  }
}
bool TextureDirtyTracker::consume_dirty(std::uint64_t key) noexcept {
  const std::scoped_lock lock(mutex_);
  const auto it = entries_.find(key);
  if (it == entries_.end() || !it->second.dirty) return false;
  it->second.dirty = false;
  return true;
}
bool TextureDirtyTracker::is_dirty(std::uint64_t key) const noexcept {
  const std::scoped_lock lock(mutex_);
  const auto it = entries_.find(key);
  return it != entries_.end() && it->second.dirty;
}

}  // namespace xenon::gpu
