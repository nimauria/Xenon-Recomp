#include "xenon/gpu/texture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <cmath>

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

std::optional<std::uint8_t> raw_resolve_texture_format(
    ColorRenderTargetFormat format) noexcept {
  switch (storage_color_format(format)) {
    case ColorRenderTargetFormat::R8G8B8A8: return std::uint8_t{6};
    case ColorRenderTargetFormat::R8G8B8A8Gamma: return std::uint8_t{62};
    case ColorRenderTargetFormat::R10G10B10A2: return std::uint8_t{7};
    case ColorRenderTargetFormat::R10G10B10A2Float: return std::uint8_t{63};
    case ColorRenderTargetFormat::R16G16Fixed: return std::uint8_t{13};
    case ColorRenderTargetFormat::R16G16B16A16Fixed: return std::uint8_t{21};
    case ColorRenderTargetFormat::R16G16Float: return std::uint8_t{31};
    case ColorRenderTargetFormat::R16G16B16A16Float:
      return std::uint8_t{32};
    case ColorRenderTargetFormat::R32Float: return std::uint8_t{36};
    case ColorRenderTargetFormat::R32G32Float: return std::uint8_t{37};
    default: return std::nullopt;
  }
}

std::uint8_t depth_resolve_texture_format(
    DepthRenderTargetFormat format) noexcept {
  return format == DepthRenderTargetFormat::D24FS8 ? std::uint8_t{23}
                                                   : std::uint8_t{22};
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

bool encode_texture(const TextureDescriptor& descriptor,
                    std::span<const std::byte> linear_data,
                    std::span<std::byte> physical_memory,
                    std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    return false;
  };
  const auto layout = build_texture_layout(descriptor);
  if (!layout.valid) return fail(layout.error);
  if (!layout.format.host_supported())
    return fail("Xenos texture format has no lossless native mapping");
  if (linear_data.size() < layout.linear_size_bytes)
    return fail("linear texture source is smaller than the declared layout");

  const auto bpb = layout.format.bytes_per_block();
  for (const auto& sub : layout.subresources) {
    for (std::uint32_t z = 0; z < sub.depth_texels; ++z) {
      for (std::uint32_t y = 0; y < sub.height_blocks; ++y) {
        for (std::uint32_t x = 0; x < sub.width_blocks; ++x) {
          const auto dx = x + sub.packed_x_blocks;
          const auto dy = y + sub.packed_y_blocks;
          const auto dz = z + sub.packed_z;
          std::uint64_t destination_offset{};
          if (descriptor.tiled) {
            destination_offset = descriptor.dimension == TextureDimension::ThreeD
                ? tiled_offset_3d(dx, dy, dz, sub.storage_pitch_blocks,
                                  sub.storage_height_blocks, bpb)
                : tiled_offset_2d(dx, dy, sub.storage_pitch_blocks, bpb);
          } else {
            destination_offset =
                std::uint64_t(dz) * sub.storage_height_blocks *
                    sub.storage_pitch_blocks * bpb +
                std::uint64_t(dy) * sub.storage_pitch_blocks * bpb +
                std::uint64_t(dx) * bpb;
          }
          const auto destination =
              std::uint64_t(sub.guest_address) + destination_offset;
          const auto source = sub.linear_offset_bytes +
              (std::uint64_t(z) * sub.height_blocks + y) *
                  sub.linear_row_pitch_bytes +
              std::uint64_t(x) * bpb;
          if (destination + bpb > physical_memory.size() ||
              source + bpb > linear_data.size()) {
            return fail("Xenos texture write references memory outside its bounds");
          }
          endian_copy(physical_memory.data() + destination,
                      linear_data.data() + source, bpb, descriptor.endian);
        }
      }
    }
  }
  if (error) error->clear();
  return true;
}

void apply_endian128(std::span<std::byte> data, Endian128 endian) noexcept {
  const auto reverse_groups = [](std::span<std::byte, 16> block,
                                 std::size_t group) {
    for (std::size_t base = 0; base < block.size(); base += group)
      std::reverse(block.begin() + static_cast<std::ptrdiff_t>(base),
                   block.begin() + static_cast<std::ptrdiff_t>(base + group));
  };
  for (std::size_t offset = 0; offset + 16 <= data.size(); offset += 16) {
    std::span<std::byte, 16> block(data.data() + offset, 16);
    switch (endian) {
      case Endian128::None:
        break;
      case Endian128::Swap8In16:
        reverse_groups(block, 2);
        break;
      case Endian128::Swap8In32:
        reverse_groups(block, 4);
        break;
      case Endian128::Swap16In32:
        for (std::size_t base = 0; base < block.size(); base += 4) {
          std::swap(block[base], block[base + 2]);
          std::swap(block[base + 1], block[base + 3]);
        }
        break;
      case Endian128::Swap8In64:
        reverse_groups(block, 8);
        break;
      case Endian128::Swap8In128:
        std::reverse(block.begin(), block.end());
        break;
    }
  }
}

ResolveWriteResult write_raw_resolve(
    const CopyResolveState& copy, const ResolveRectangle& rectangle,
    std::span<const std::byte> source, std::uint32_t source_row_pitch,
    std::span<std::byte> physical_memory) {
  ResolveWriteResult result{};
  if (copy.command != CopyCommand::Raw) {
    result.error = "converted Xenos resolve requires format conversion";
    return result;
  }
  if (!rectangle.valid || rectangle.left < 0 || rectangle.top < 0) {
    result.error = "invalid Xenos resolve rectangle";
    return result;
  }
  if (rectangle.empty()) {
    result.valid = true;
    return result;
  }
  if (!copy.destination_pitch ||
      (copy.destination_array && !copy.destination_height)) {
    result.error = "raw resolve destination storage dimensions are invalid";
    return result;
  }
  if (copy.destination_red_blue_swap || copy.destination_exponent_bias) {
    result.error = "raw resolve cannot apply component swap or exponent bias";
    return result;
  }
  const auto& format = texture_format_info(copy.destination_format);
  if (format.storage != TextureStorage::Uncompressed ||
      format.block_width != 1 || format.block_height != 1) {
    result.error = "raw resolve destination format is not losslessly writable";
    return result;
  }
  const auto bytes_per_pixel = format.bytes_per_block();
  const auto width = static_cast<std::uint32_t>(rectangle.right - rectangle.left);
  const auto height = static_cast<std::uint32_t>(rectangle.bottom - rectangle.top);
  if (source_row_pitch < width * bytes_per_pixel ||
      std::uint64_t(source_row_pitch) * height > source.size()) {
    result.error = "native resolve readback is smaller than its rectangle";
    return result;
  }
  const auto pitch_aligned =
      (std::uint32_t(copy.destination_pitch) + 31u) & ~31u;
  const auto height_aligned =
      (std::uint32_t(copy.destination_height) + 31u) & ~31u;
  std::map<std::uint32_t, std::array<std::byte, 16>> blocks;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto destination_x =
          static_cast<std::uint32_t>(rectangle.left) + x;
      const auto destination_y =
          static_cast<std::uint32_t>(rectangle.top) + y;
      // Pitch and height describe tiled storage, not clipping dimensions.
      // In array mode the base is already adjusted to its Z/8 group and the
      // three-bit destination slice selects the layer within that group.
      const auto tiled = copy.destination_array
          ? tiled_offset_3d(destination_x, destination_y,
                            copy.destination_slice, pitch_aligned,
                            height_aligned, bytes_per_pixel)
          : tiled_offset_2d(destination_x, destination_y, pitch_aligned,
                            bytes_per_pixel);
      const auto destination = std::uint64_t(copy.destination_base) + tiled;
      if (destination + bytes_per_pixel > physical_memory.size()) {
        result.error = "raw resolve destination is outside physical memory";
        return result;
      }
      const auto block_address = static_cast<std::uint32_t>(destination & ~15ull);
      auto [block_it, inserted] = blocks.try_emplace(block_address);
      if (inserted) {
        if (std::uint64_t(block_address) + 16u > physical_memory.size()) {
          result.error = "raw resolve endian block is outside physical memory";
          return result;
        }
        std::memcpy(block_it->second.data(),
                    physical_memory.data() + block_address, 16);
        apply_endian128(block_it->second, copy.destination_endian);
      }
      const auto block_offset = static_cast<std::size_t>(destination & 15ull);
      if (block_offset + bytes_per_pixel > 16u) {
        result.error = "raw resolve pixel crosses a 128-bit endian block";
        return result;
      }
      std::memcpy(block_it->second.data() + block_offset,
                  source.data() + std::size_t(y) * source_row_pitch +
                      std::size_t(x) * bytes_per_pixel,
                  bytes_per_pixel);
    }
  }
  if (blocks.empty()) {
    result.error = "raw resolve produced no destination blocks";
    return result;
  }
  for (auto& [address, block] : blocks) {
    apply_endian128(block, copy.destination_endian);
    std::memcpy(physical_memory.data() + address, block.data(), block.size());
  }
  const auto first = blocks.begin()->first;
  const auto last = blocks.rbegin()->first + 16u;
  result.modified_address = first;
  result.modified_size = last - first;
  result.valid = true;
  return result;
}

ResolveWriteResult write_converted_resolve(
    const CopyResolveState& copy, ColorRenderTargetFormat source_format,
    const ResolveRectangle& rectangle, std::span<const std::byte> source,
    std::uint32_t source_row_pitch, std::span<std::byte> physical_memory) {
  ResolveWriteResult result{};
  if (copy.command != CopyCommand::Convert && copy.command != CopyCommand::Raw) {
    result.error = "Xenos resolve command cannot use color conversion";
    return result;
  }
  if (!rectangle.valid || rectangle.left < 0 || rectangle.top < 0) {
    result.error = "invalid converted Xenos resolve rectangle";
    return result;
  }
  if (rectangle.empty()) { result.valid = true; return result; }
  const auto& destination_format = texture_format_info(copy.destination_format);
  if (destination_format.storage != TextureStorage::Uncompressed ||
      destination_format.block_width != 1 ||
      destination_format.block_height != 1 ||
      !destination_format.bits_per_pixel ||
      (destination_format.bits_per_pixel & 7u)) {
    result.error = "converted resolve destination format is unsupported";
    return result;
  }
  const auto source_bytes = color_host_bytes_per_pixel(source_format);
  const auto destination_bytes = destination_format.bytes_per_block();
  const auto width = std::uint32_t(rectangle.right - rectangle.left);
  const auto height = std::uint32_t(rectangle.bottom - rectangle.top);
  if (!source_bytes || source_row_pitch < width * source_bytes ||
      std::uint64_t(source_row_pitch) * height > source.size()) {
    result.error = "converted resolve source is smaller than its rectangle";
    return result;
  }
  std::vector<std::byte> converted(
      std::size_t(width) * height * destination_bytes);
  const auto unorm = [](float value, std::uint32_t maximum) {
    return std::uint32_t(std::nearbyint(
        std::clamp(value, 0.0f, 1.0f) * float(maximum)));
  };
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      ColorSample sample{};
      if (!decode_host_color_sample(
              source_format,
              source.subspan(std::uint64_t(y) * source_row_pitch +
                                  std::uint64_t(x) * source_bytes,
                              source_bytes), sample)) {
        result.error = "converted resolve could not decode native source";
        return result;
      }
      auto& c = sample.components;
      for (auto& component : c)
        component = std::ldexp(component, copy.destination_exponent_bias);
      if (copy.destination_red_blue_swap) std::swap(c[0], c[2]);
      auto destination = std::span(converted).subspan(
          (std::size_t(y) * width + x) * destination_bytes,
          destination_bytes);
      std::array<std::uint32_t, 4> words{};
      switch (copy.destination_format) {
        case 2: words[0] = unorm(c[0], 255u); break;
        case 6: case 14: case 50:
          for (unsigned i = 0; i < 4; ++i)
            words[0] |= unorm(c[i], 255u) << (i * 8u);
          break;
        case 62:
          words = {pack_color_sample(
              ColorRenderTargetFormat::R8G8B8A8Gamma, sample)[0], 0, 0, 0};
          break;
        case 7: case 54:
          words[0] = unorm(c[0], 1023u) | (unorm(c[1], 1023u) << 10u) |
                     (unorm(c[2], 1023u) << 20u) |
                     (unorm(c[3], 3u) << 30u);
          break;
        case 63:
          words = {pack_color_sample(
              ColorRenderTargetFormat::R10G10B10A2Float, sample)[0], 0, 0, 0};
          break;
        case 10:
          words[0] = unorm(c[0], 255u) | (unorm(c[1], 255u) << 8u);
          break;
        case 13: {
          const auto packed = pack_color_sample(
              ColorRenderTargetFormat::R16G16Fixed, sample);
          words[0] = packed[0];
          break;
        }
        case 21: {
          const auto packed = pack_color_sample(
              ColorRenderTargetFormat::R16G16B16A16Fixed, sample);
          words[0] = packed[0]; words[1] = packed[1];
          break;
        }
        case 25:
          words[0] = unorm(c[0], 65535u) | (unorm(c[1], 65535u) << 16u);
          break;
        case 26:
          words[0] = unorm(c[0], 65535u) | (unorm(c[1], 65535u) << 16u);
          words[1] = unorm(c[2], 65535u) | (unorm(c[3], 65535u) << 16u);
          break;
        case 30: case 31: case 32: {
          const auto packed = pack_color_sample(
              copy.destination_format == 32
                  ? ColorRenderTargetFormat::R16G16B16A16Float
                  : ColorRenderTargetFormat::R16G16Float,
              sample);
          words[0] = packed[0]; words[1] = packed[1];
          break;
        }
        case 36:
          words[0] = std::bit_cast<std::uint32_t>(c[0]); break;
        case 37:
          words[0] = std::bit_cast<std::uint32_t>(c[0]);
          words[1] = std::bit_cast<std::uint32_t>(c[1]); break;
        case 38:
          for (unsigned i = 0; i < 4; ++i)
            words[i] = std::bit_cast<std::uint32_t>(c[i]);
          break;
        default:
          result.error = "converted resolve destination codec is unsupported";
          return result;
      }
      std::memcpy(destination.data(), words.data(), destination_bytes);
    }
  }
  auto raw_copy = copy;
  raw_copy.command = CopyCommand::Raw;
  raw_copy.destination_exponent_bias = 0;
  raw_copy.destination_red_blue_swap = false;
  return write_raw_resolve(raw_copy, rectangle, converted,
                           width * destination_bytes, physical_memory);
}

ResolveWriteResult write_depth_resolve(
    const CopyResolveState& copy, DepthRenderTargetFormat source_format,
    const ResolveRectangle& rectangle, std::span<const std::uint32_t> source,
    std::uint32_t source_row_pitch, std::span<std::byte> physical_memory) {
  ResolveWriteResult result{};
  if (copy.command != CopyCommand::Raw && copy.command != CopyCommand::Convert) {
    result.error = "Xenos depth resolve command is unsupported";
    return result;
  }

  // Direct3D 9 commonly programs the color-style 8_8_8_8 destination format
  // for depth copies. Xenos actually stores the selected depth sample in the
  // depth target's native 24_8 / 24_8_FLOAT bit layout, with stencil intact.
  auto raw_copy = copy;
  raw_copy.command = CopyCommand::Raw;
  raw_copy.destination_format = depth_resolve_texture_format(source_format);
  raw_copy.destination_exponent_bias = 0;
  raw_copy.destination_red_blue_swap = false;
  return write_raw_resolve(raw_copy, rectangle, std::as_bytes(source),
                           source_row_pitch, physical_memory);
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
void TextureDirtyTracker::track_clean(std::uint64_t key,
                                      const TextureLayout& layout,
                                      std::uint64_t clean_epoch) {
  const std::scoped_lock lock(mutex_);
  Entry entry{};
  entry.dirty = false;
  entry.clean_epoch = clean_epoch;
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
bool TextureDirtyTracker::consume_dirty(
    std::uint64_t key, const memory::GuestMemoryCoherency& coherency,
    std::uint64_t through_epoch) noexcept {
  const std::scoped_lock lock(mutex_);
  const auto it = entries_.find(key);
  if (it == entries_.end()) return false;
  auto& entry = it->second;
  bool dirty = entry.dirty;
  if (!dirty) {
    for (const auto& range : entry.ranges) {
      if (range.first >= memory::kPhysicalMemorySize) continue;
      const auto width64 = std::min<std::uint64_t>(
          range.second - range.first, memory::kPhysicalMemorySize - range.first);
      if (width64 && coherency.range_changed_since(
                         static_cast<std::uint32_t>(range.first),
                         static_cast<std::uint32_t>(width64),
                         entry.clean_epoch, through_epoch)) {
        dirty = true;
        break;
      }
    }
  }
  entry.dirty = false;
  entry.clean_epoch = through_epoch;
  return dirty;
}
bool TextureDirtyTracker::is_dirty(std::uint64_t key) const noexcept {
  const std::scoped_lock lock(mutex_);
  const auto it = entries_.find(key);
  return it != entries_.end() && it->second.dirty;
}

}  // namespace xenon::gpu
