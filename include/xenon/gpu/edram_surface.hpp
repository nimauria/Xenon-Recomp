#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "xenon/gpu/edram.hpp"

namespace xenon::gpu {

enum class MsaaSamples : std::uint8_t { X1 = 0, X2 = 1, X4 = 2 };
enum class EdramMode : std::uint8_t {
  NoOperation = 0,
  ColorDepth = 4,
  DepthOnly = 5,
  Copy = 6,
};

enum class ColorRenderTargetFormat : std::uint8_t {
  R8G8B8A8 = 0,
  R8G8B8A8Gamma = 1,
  R10G10B10A2 = 2,
  R10G10B10A2Float = 3,
  R16G16Fixed = 4,
  R16G16B16A16Fixed = 5,
  R16G16Float = 6,
  R16G16B16A16Float = 7,
  R10G10B10A2As10 = 10,
  R10G10B10A2FloatAs16 = 12,
  R32Float = 14,
  R32G32Float = 15,
};

enum class DepthRenderTargetFormat : std::uint8_t { D24S8 = 0, D24FS8 = 1 };

struct EdramSurfaceLayout {
  std::uint32_t base_tile{};
  std::uint32_t pitch_pixels{};
  std::uint32_t height_pixels{};
  MsaaSamples msaa{MsaaSamples::X1};
  bool is_64bpp{};
  bool depth{};

  [[nodiscard]] std::uint32_t sample_width() const noexcept;
  [[nodiscard]] std::uint32_t sample_height() const noexcept;
  [[nodiscard]] std::uint32_t pitch_tiles() const noexcept;
  [[nodiscard]] std::uint32_t row_tiles() const noexcept;
  [[nodiscard]] std::uint32_t tile_rows() const noexcept;
  [[nodiscard]] std::uint32_t tile_count() const noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint64_t hash() const noexcept;
};

struct EdramSampleAddress {
  std::uint32_t low{};
  std::uint32_t high{};
  bool has_high{};
};

[[nodiscard]] bool color_render_target_is_64bpp(
    ColorRenderTargetFormat format) noexcept;
[[nodiscard]] std::optional<EdramSampleAddress> edram_sample_address(
    const EdramSurfaceLayout& layout, std::uint32_t x, std::uint32_t y,
    std::uint32_t sample = 0) noexcept;
[[nodiscard]] std::array<std::uint32_t, 2> read_edram_sample(
    const Edram& edram, const EdramSurfaceLayout& layout, std::uint32_t x,
    std::uint32_t y, std::uint32_t sample = 0) noexcept;
[[nodiscard]] bool write_edram_sample(
    Edram& edram, const EdramSurfaceLayout& layout, std::uint32_t x,
    std::uint32_t y, std::array<std::uint32_t, 2> value,
    std::uint32_t sample = 0) noexcept;

// Copies one selected sample per pixel into a tightly packed host buffer.
// This raw path is the lossless basis for format conversion and guest-memory
// resolves; 64bpp samples contain two consecutive little-endian dwords.
[[nodiscard]] bool resolve_edram_raw(
    const Edram& edram, const EdramSurfaceLayout& layout,
    std::uint32_t left, std::uint32_t top, std::uint32_t width,
    std::uint32_t height, std::uint32_t sample,
    std::span<std::byte> destination, std::uint32_t destination_pitch) noexcept;

}  // namespace xenon::gpu
