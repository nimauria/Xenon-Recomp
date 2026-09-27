#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

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
  // Stable native-resource identity. Height is deliberately excluded because
  // it is a drawable extent (often derived from the current scissor), not a
  // Xenos render-target identity field.
  [[nodiscard]] std::uint64_t identity_hash() const noexcept;
  [[nodiscard]] std::uint64_t hash() const noexcept;
};

struct EdramSampleAddress {
  std::uint32_t low{};
  std::uint32_t high{};
  bool has_high{};
};

struct EdramSurfaceRegion {
  std::int32_t left{};
  std::int32_t top{};
  std::int32_t right{};
  std::int32_t bottom{};
};

[[nodiscard]] std::vector<std::uint16_t> covered_edram_tiles(
    const EdramSurfaceLayout& layout, EdramSurfaceRegion region);

struct HostSampleMapping {
  std::uint32_t sample{};
  std::uint32_t sample_count{1};
};

// Maps the spatial Xenos sample order to the D3D10+/Vulkan standard sample
// order. Native 2x reverses the two indices; hosts without 2x attachments use
// the top-left and bottom-right samples (0 and 3) of a 4x attachment.
[[nodiscard]] std::optional<HostSampleMapping> map_guest_sample_to_host(
    MsaaSamples samples, std::uint32_t guest_sample,
    bool native_2x_supported = true) noexcept;

[[nodiscard]] bool color_render_target_is_64bpp(
    ColorRenderTargetFormat format) noexcept;
[[nodiscard]] ColorRenderTargetFormat storage_color_format(
    ColorRenderTargetFormat format) noexcept;

[[nodiscard]] float xenos_pwl_gamma_to_linear(float gamma) noexcept;
[[nodiscard]] float xenos_linear_to_pwl_gamma(float linear) noexcept;
[[nodiscard]] float float7e3_to_float32(std::uint32_t value) noexcept;
[[nodiscard]] std::uint32_t float32_to_7e3(float value) noexcept;

struct ColorSample {
  std::array<float, 4> components{};
};

enum class ColorHostStorage : std::uint8_t {
  Unsupported,
  R8G8B8A8Unorm,
  R16G16B16A16Unorm,
  R10G10B10A2Unorm,
  R16G16Float,
  R16G16B16A16Float,
  R32Float,
  R32G32Float,
};

[[nodiscard]] ColorHostStorage color_host_storage(
    ColorRenderTargetFormat format) noexcept;
[[nodiscard]] std::uint32_t color_host_bytes_per_pixel(
    ColorRenderTargetFormat format) noexcept;
[[nodiscard]] bool color_host_requires_conversion(
    ColorRenderTargetFormat format) noexcept;
[[nodiscard]] bool encode_host_color_sample(
    ColorRenderTargetFormat format, const ColorSample& sample,
    std::span<std::byte> destination) noexcept;
[[nodiscard]] bool decode_host_color_sample(
    ColorRenderTargetFormat format, std::span<const std::byte> source,
    ColorSample& sample) noexcept;
// Averages two native host-storage color readbacks in linear component space.
// Xenos Samples01 / Samples23 resolves average exactly two guest samples; the
// backends use this after explicitly reading those samples instead of asking a
// native full-MSAA resolve to average unrelated samples too.
[[nodiscard]] bool average_host_color_samples(
    ColorRenderTargetFormat format, std::uint32_t width,
    std::uint32_t height, std::span<const std::byte> source_a,
    std::uint32_t source_a_pitch, std::span<const std::byte> source_b,
    std::uint32_t source_b_pitch, std::vector<std::byte>& destination,
    std::uint32_t& destination_pitch) noexcept;
[[nodiscard]] bool edram_color_to_host(
    ColorRenderTargetFormat format, std::uint32_t width,
    std::uint32_t height, std::span<const std::byte> source,
    std::uint32_t source_pitch, std::span<std::byte> destination,
    std::uint32_t destination_pitch) noexcept;
[[nodiscard]] bool host_color_to_edram(
    ColorRenderTargetFormat format, std::uint32_t width,
    std::uint32_t height, std::span<const std::byte> source,
    std::uint32_t source_pitch, std::span<std::byte> destination,
    std::uint32_t destination_pitch) noexcept;

// Converts the exact Xenos EDRAM bit representation to/from linear float
// components. These are the common authority for resolves and native surface
// ownership; host formats are transport storage only.
[[nodiscard]] ColorSample unpack_color_sample(
    ColorRenderTargetFormat format,
    std::array<std::uint32_t, 2> packed) noexcept;
[[nodiscard]] std::array<std::uint32_t, 2> pack_color_sample(
    ColorRenderTargetFormat format, const ColorSample& sample) noexcept;
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

// Writes every guest sample in the clipped pixel rectangle. Resolve clears
// are regional Xenos EDRAM operations, not whole native-attachment clears.
[[nodiscard]] bool clear_edram_surface_region(
    Edram& edram, const EdramSurfaceLayout& layout, std::int32_t left,
    std::int32_t top, std::int32_t right, std::int32_t bottom,
    std::array<std::uint32_t, 2> value) noexcept;

// Copies one selected sample per pixel into a tightly packed host buffer.
// This raw path is the lossless basis for format conversion and guest-memory
// resolves; 64bpp samples contain two consecutive little-endian dwords.
[[nodiscard]] bool resolve_edram_raw(
    const Edram& edram, const EdramSurfaceLayout& layout,
    std::uint32_t left, std::uint32_t top, std::uint32_t width,
    std::uint32_t height, std::uint32_t sample,
    std::span<std::byte> destination, std::uint32_t destination_pitch) noexcept;

// Inverse of resolve_edram_raw for one selected sample. Used when a native
// surface relinquishes tile ownership back to the canonical EDRAM byte store.
[[nodiscard]] bool store_edram_raw(
    Edram& edram, const EdramSurfaceLayout& layout,
    std::uint32_t left, std::uint32_t top, std::uint32_t width,
    std::uint32_t height, std::uint32_t sample,
    std::span<const std::byte> source, std::uint32_t source_pitch) noexcept;

}  // namespace xenon::gpu
