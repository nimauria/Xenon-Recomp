#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xenon/gpu/resource_ir.hpp"
#include "xenon/memory/coherency.hpp"

namespace xenon::memory {
class AddressSpace;
}

namespace xenon::gpu {

enum class TextureStorage : std::uint8_t { Uncompressed, BlockCompressed, Packed };

enum class TextureHostFormat : std::uint8_t {
  Unsupported,
  R8Unorm,
  R8G8Unorm,
  R8G8B8A8Unorm,
  R8G8B8A8Srgb,
  B5G5R5A1Unorm,
  B5G6R5Unorm,
  B4G4R4A4Unorm,
  R10G10B10A2Unorm,
  R11G11B10Float,
  R16Unorm,
  R16G16Unorm,
  R16G16B16A16Unorm,
  R16Float,
  R16G16Float,
  R16G16B16A16Float,
  R32Uint,
  R32G32Uint,
  R32G32B32A32Uint,
  R32Float,
  R32G32Float,
  R32G32B32Float,
  R32G32B32A32Float,
  D24UnormS8Uint,
  BC1Unorm,
  BC2Unorm,
  BC3Unorm,
  BC4Unorm,
  BC5Unorm,
};

struct TextureFormatInfo {
  std::uint8_t xenos_format{};
  std::string_view name{};
  TextureStorage storage{TextureStorage::Uncompressed};
  std::uint8_t block_width{1};
  std::uint8_t block_height{1};
  std::uint8_t bits_per_pixel{};
  TextureHostFormat host_format{TextureHostFormat::Unsupported};
  [[nodiscard]] constexpr std::uint32_t bytes_per_block() const noexcept {
    return (std::uint32_t(bits_per_pixel) * block_width * block_height + 7u) / 8u;
  }
  [[nodiscard]] constexpr bool host_supported() const noexcept {
    return host_format != TextureHostFormat::Unsupported;
  }
};

[[nodiscard]] const TextureFormatInfo& texture_format_info(
    std::uint8_t format) noexcept;

// Returns the texture-format encoding whose in-memory bits exactly match a
// native color-target readback. Formats requiring numeric conversion are not
// raw-compatible and return no value.
[[nodiscard]] std::optional<std::uint8_t> raw_resolve_texture_format(
    ColorRenderTargetFormat format) noexcept;

// Xenos depth resolves ignore the color-style copy destination format field.
// The destination bit layout is always the actual depth render-target format.
[[nodiscard]] std::uint8_t depth_resolve_texture_format(
    DepthRenderTargetFormat format) noexcept;

struct TextureSubresourceLayout {
  std::uint32_t mip_level{};
  std::uint32_t array_layer{};
  std::uint32_t guest_address{};
  std::uint32_t width_texels{};
  std::uint32_t height_texels{};
  std::uint32_t depth_texels{};
  std::uint32_t width_blocks{};
  std::uint32_t height_blocks{};
  std::uint32_t storage_pitch_blocks{};
  std::uint32_t storage_height_blocks{};
  std::uint32_t packed_x_blocks{};
  std::uint32_t packed_y_blocks{};
  std::uint32_t packed_z{};
  std::uint64_t guest_size_bytes{};
  std::uint64_t linear_offset_bytes{};
  std::uint32_t linear_row_pitch_bytes{};
};

struct TextureLayout {
  TextureDescriptor descriptor{};
  TextureFormatInfo format{};
  std::vector<TextureSubresourceLayout> subresources{};
  std::uint64_t base_extent_bytes{};
  std::uint64_t mip_extent_bytes{};
  std::uint64_t linear_size_bytes{};
  std::string error{};
  bool valid{};
};

[[nodiscard]] std::uint64_t tiled_offset_2d(
    std::uint32_t x_blocks, std::uint32_t y_blocks,
    std::uint32_t pitch_blocks_aligned,
    std::uint32_t bytes_per_block) noexcept;
[[nodiscard]] std::uint64_t tiled_offset_3d(
    std::uint32_t x_blocks, std::uint32_t y_blocks, std::uint32_t z,
    std::uint32_t pitch_blocks_aligned,
    std::uint32_t height_blocks_aligned,
    std::uint32_t bytes_per_block) noexcept;

[[nodiscard]] TextureLayout build_texture_layout(
    const TextureDescriptor& descriptor);

struct DecodedTexture {
  TextureLayout layout{};
  std::vector<std::byte> linear_data{};
  std::string error{};
  bool valid{};
};

[[nodiscard]] DecodedTexture decode_texture(
    const TextureDescriptor& descriptor,
    std::span<const std::byte> physical_memory);

// Inverse of decode_texture, used by EDRAM resolves and CPU-visible readbacks.
// The source is tightly packed in the same subresource order as
// TextureLayout::subresources; the destination receives Xenos tiling and
// endian conversion.
[[nodiscard]] bool encode_texture(
    const TextureDescriptor& descriptor, std::span<const std::byte> linear_data,
    std::span<std::byte> physical_memory, std::string* error = nullptr);

// Applies the RB copy/memexport 128-bit endian mode independently to every
// complete 16-byte group. All defined Xenos modes are involutions.
void apply_endian128(std::span<std::byte> data, Endian128 endian) noexcept;

struct ResolveWriteResult {
  std::uint32_t modified_address{};
  std::uint32_t modified_size{};
  std::string error{};
  bool valid{};
};

// Writes an already format-compatible native readback rectangle to the Xenos
// tiled copy destination. Conversion, red/blue swap and exponent bias are
// intentionally rejected here and belong to the converted-resolve path.
[[nodiscard]] ResolveWriteResult write_raw_resolve(
    const CopyResolveState& copy, const ResolveRectangle& rectangle,
    std::span<const std::byte> source, std::uint32_t source_row_pitch,
    memory::AddressSpace& physical_memory);

// Converts linear native render-target pixels through the common Xenos color
// model, then writes the guest tiled destination with copy endian semantics.
[[nodiscard]] ResolveWriteResult write_converted_resolve(
    const CopyResolveState& copy, ColorRenderTargetFormat source_format,
    const ResolveRectangle& rectangle, std::span<const std::byte> source,
    std::uint32_t source_row_pitch,
    memory::AddressSpace& physical_memory);

// Writes exact canonical Xenos D24S8 / D24FS8 words to the copy destination.
// Depth never performs color conversion or sample averaging; the caller passes
// the single sample selected by the backend-neutral resolve plan.
[[nodiscard]] ResolveWriteResult write_depth_resolve(
    const CopyResolveState& copy, DepthRenderTargetFormat source_format,
    const ResolveRectangle& rectangle, std::span<const std::uint32_t> source,
    std::uint32_t source_row_pitch,
    memory::AddressSpace& physical_memory);

class TextureDirtyTracker {
 public:
  void track(std::uint64_t key, const TextureLayout& layout);
  void track_clean(std::uint64_t key, const TextureLayout& layout,
                   std::uint64_t clean_epoch);
  void erase(std::uint64_t key);
  void clear();
  void mark_dirty(std::uint32_t address, std::uint32_t size) noexcept;
  [[nodiscard]] bool consume_dirty(std::uint64_t key) noexcept;
  [[nodiscard]] bool consume_dirty(
      std::uint64_t key, const memory::GuestMemoryCoherency& coherency,
      std::uint64_t through_epoch) noexcept;
  [[nodiscard]] bool is_dirty(std::uint64_t key) const noexcept;

 private:
  struct Entry {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges{};
    bool dirty{true};
    std::uint64_t clean_epoch{};
  };
  std::unordered_map<std::uint64_t, Entry> entries_{};
  mutable std::mutex mutex_{};
};

}  // namespace xenon::gpu
