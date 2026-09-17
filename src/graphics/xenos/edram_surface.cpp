#include "xenon/gpu/edram_surface.hpp"

#include <algorithm>
#include <cstring>

namespace xenon::gpu {
namespace {

std::uint32_t read_u32(const Edram& edram, std::uint32_t address) noexcept {
  std::array<std::byte, 4> bytes{};
  edram.read(address, bytes);
  std::uint32_t value{};
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

void write_u32(Edram& edram, std::uint32_t address,
               std::uint32_t value) noexcept {
  std::array<std::byte, 4> bytes{};
  std::memcpy(bytes.data(), &value, sizeof(value));
  edram.write(address, bytes);
}

}  // namespace

std::uint32_t EdramSurfaceLayout::sample_width() const noexcept {
  return pitch_pixels << (msaa == MsaaSamples::X4 ? 1u : 0u);
}

std::uint32_t EdramSurfaceLayout::sample_height() const noexcept {
  return height_pixels << (msaa == MsaaSamples::X1 ? 0u : 1u);
}

std::uint32_t EdramSurfaceLayout::pitch_tiles() const noexcept {
  const auto tiles = (sample_width() + 79u) / 80u;
  return is_64bpp ? tiles * 2u : tiles;
}

std::uint32_t EdramSurfaceLayout::row_tiles() const noexcept {
  return pitch_tiles();
}

std::uint32_t EdramSurfaceLayout::tile_rows() const noexcept {
  return (sample_height() + 15u) / 16u;
}

std::uint32_t EdramSurfaceLayout::tile_count() const noexcept {
  return pitch_tiles() * tile_rows();
}

bool EdramSurfaceLayout::valid() const noexcept {
  return pitch_pixels && height_pixels &&
         static_cast<unsigned>(msaa) <= static_cast<unsigned>(MsaaSamples::X4) &&
         pitch_tiles() && tile_count();
}

std::uint64_t EdramSurfaceLayout::hash() const noexcept {
  std::uint64_t hash = 14695981039346656037ull;
  auto add = [&](std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
      hash ^= static_cast<std::uint8_t>(value >> (i * 8u));
      hash *= 1099511628211ull;
    }
  };
  add(base_tile); add(pitch_pixels); add(height_pixels);
  add(static_cast<std::uint8_t>(msaa)); add(is_64bpp); add(depth);
  return hash;
}

bool color_render_target_is_64bpp(ColorRenderTargetFormat format) noexcept {
  return format == ColorRenderTargetFormat::R16G16B16A16Fixed ||
         format == ColorRenderTargetFormat::R16G16B16A16Float ||
         format == ColorRenderTargetFormat::R32G32Float;
}

std::optional<EdramSampleAddress> edram_sample_address(
    const EdramSurfaceLayout& layout, std::uint32_t x, std::uint32_t y,
    std::uint32_t sample) noexcept {
  if (!layout.valid() || x >= layout.pitch_pixels || y >= layout.height_pixels)
    return std::nullopt;
  const auto sample_count = 1u << static_cast<unsigned>(layout.msaa);
  if (sample >= sample_count) return std::nullopt;

  const auto sample_x = (x << (layout.msaa == MsaaSamples::X4 ? 1u : 0u)) |
                        (layout.msaa == MsaaSamples::X4 ? (sample & 1u) : 0u);
  const auto sample_y = (y << (layout.msaa == MsaaSamples::X1 ? 0u : 1u)) |
                        (layout.msaa == MsaaSamples::X1 ? 0u :
                         layout.msaa == MsaaSamples::X2 ? sample : sample >> 1u);
  const auto tile_x = sample_x / 80u;
  const auto tile_y = sample_y / 16u;
  auto in_tile_x = sample_x % 80u;
  // Xenos depth tiles exchange their two 40-sample halves.
  if (layout.depth) in_tile_x = (in_tile_x + 40u) % 80u;
  const auto word = (sample_y % 16u) * 80u + in_tile_x;
  const auto tile_stride = layout.is_64bpp ? 2u : 1u;
  const auto tile = (layout.base_tile + tile_y * layout.pitch_tiles() +
                     tile_x * tile_stride) % Edram::kTileCount;
  EdramSampleAddress result{};
  result.low = tile * Edram::kTileBytes + word * 4u;
  result.has_high = layout.is_64bpp;
  if (result.has_high) {
    const auto high_tile = (tile + 1u) % Edram::kTileCount;
    result.high = high_tile * Edram::kTileBytes + word * 4u;
  }
  return result;
}

std::array<std::uint32_t, 2> read_edram_sample(
    const Edram& edram, const EdramSurfaceLayout& layout, std::uint32_t x,
    std::uint32_t y, std::uint32_t sample) noexcept {
  const auto address = edram_sample_address(layout, x, y, sample);
  if (!address) return {};
  return {read_u32(edram, address->low),
          address->has_high ? read_u32(edram, address->high) : 0u};
}

bool write_edram_sample(Edram& edram, const EdramSurfaceLayout& layout,
                        std::uint32_t x, std::uint32_t y,
                        std::array<std::uint32_t, 2> value,
                        std::uint32_t sample) noexcept {
  const auto address = edram_sample_address(layout, x, y, sample);
  if (!address) return false;
  write_u32(edram, address->low, value[0]);
  if (address->has_high) write_u32(edram, address->high, value[1]);
  return true;
}

bool resolve_edram_raw(const Edram& edram, const EdramSurfaceLayout& layout,
                       std::uint32_t left, std::uint32_t top,
                       std::uint32_t width, std::uint32_t height,
                       std::uint32_t sample, std::span<std::byte> destination,
                       std::uint32_t destination_pitch) noexcept {
  const auto pixel_bytes = layout.is_64bpp ? 8u : 4u;
  if (!width || !height || left + width > layout.pitch_pixels ||
      top + height > layout.height_pixels ||
      destination_pitch < width * pixel_bytes ||
      std::uint64_t(destination_pitch) * height > destination.size()) return false;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto value = read_edram_sample(edram, layout, left + x, top + y, sample);
      auto* out = destination.data() + std::uint64_t(y) * destination_pitch +
                  std::uint64_t(x) * pixel_bytes;
      std::memcpy(out, &value[0], 4);
      if (layout.is_64bpp) std::memcpy(out + 4, &value[1], 4);
    }
  }
  return true;
}

}  // namespace xenon::gpu
