#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/register_file.hpp"

namespace xenon::gpu {

enum class CompareFunction : std::uint8_t {
  Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always
};
enum class StencilOperation : std::uint8_t {
  Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert,
  IncrementWrap, DecrementWrap
};
enum class BlendFactor : std::uint8_t {
  Zero = 0, One = 1, SrcColor = 4, InvSrcColor = 5, SrcAlpha = 6,
  InvSrcAlpha = 7, DestColor = 8, InvDestColor = 9, DestAlpha = 10,
  InvDestAlpha = 11, ConstantColor = 12, InvConstantColor = 13,
  ConstantAlpha = 14, InvConstantAlpha = 15, SrcAlphaSaturate = 16
};
enum class BlendOperation : std::uint8_t {
  Add, Subtract, Minimum, Maximum, ReverseSubtract
};

struct BlendState {
  BlendFactor color_source{BlendFactor::One};
  BlendFactor color_destination{BlendFactor::Zero};
  BlendOperation color_operation{BlendOperation::Add};
  BlendFactor alpha_source{BlendFactor::One};
  BlendFactor alpha_destination{BlendFactor::Zero};
  BlendOperation alpha_operation{BlendOperation::Add};
  bool enabled{};
};

struct StencilFaceState {
  CompareFunction function{CompareFunction::Always};
  StencilOperation fail{StencilOperation::Keep};
  StencilOperation depth_pass{StencilOperation::Keep};
  StencilOperation depth_fail{StencilOperation::Keep};
};

enum class TextureDimension : std::uint8_t { OneD, TwoDOrStacked, ThreeD, Cube };

struct VertexBufferDescriptor {
  std::uint32_t physical_address{};
  std::uint32_t size_dwords{};
  Endian endian{Endian::None};
  bool valid{};
  [[nodiscard]] std::uint64_t hash() const noexcept;
};

struct TextureDescriptor {
  std::uint32_t base_address{};
  std::uint32_t mip_address{};
  std::uint32_t width{};
  std::uint32_t height{1};
  std::uint32_t depth{1};
  std::uint32_t pitch{};
  std::uint8_t format{};
  Endian endian{Endian::None};
  TextureDimension dimension{TextureDimension::TwoDOrStacked};
  std::uint8_t mip_min_level{};
  std::uint8_t mip_max_level{};
  std::uint16_t swizzle{};
  std::array<std::uint8_t, 4> signs{};
  std::array<std::uint8_t, 3> clamps{};
  std::uint8_t mag_filter{};
  std::uint8_t min_filter{};
  std::uint8_t mip_filter{};
  std::uint8_t aniso_filter{};
  std::uint8_t border_color{};
  std::int16_t lod_bias{};
  bool stacked{};
  bool tiled{};
  bool packed_mips{};
  bool valid{};
  [[nodiscard]] std::uint64_t hash() const noexcept;
};

struct RenderTargetDescriptor {
  std::uint16_t base_tile{};
  std::uint8_t format{};
  std::int8_t exponent_bias{};
  std::uint8_t write_mask{};
  BlendState blend{};
  bool enabled{};
};

struct DepthTargetDescriptor {
  std::uint16_t base_tile{};
  std::uint8_t format{};
  bool test_enabled{};
  bool write_enabled{};
  CompareFunction function{CompareFunction::Always};
  StencilFaceState stencil_front{};
  StencilFaceState stencil_back{};
  std::uint8_t stencil_reference{};
  std::uint8_t stencil_read_mask{0xFF};
  std::uint8_t stencil_write_mask{0xFF};
  std::uint8_t stencil_back_reference{};
  std::uint8_t stencil_back_read_mask{0xFF};
  std::uint8_t stencil_back_write_mask{0xFF};
  bool stencil_enabled{};
  bool backface_stencil_enabled{};
};

struct ViewportState {
  float x_scale{};
  float x_offset{};
  float y_scale{};
  float y_offset{};
  float z_scale{1.0f};
  float z_offset{};
};

struct RasterState {
  std::uint16_t surface_pitch{};
  std::uint8_t msaa_samples_log2{};
  std::int32_t scissor_left{};
  std::int32_t scissor_top{};
  std::int32_t scissor_right{};
  std::int32_t scissor_bottom{};
  bool cull_front{};
  bool cull_back{};
  bool front_face_clockwise{};
  bool multisample_enabled{};
  std::uint8_t polygon_mode{};
  std::uint8_t front_polygon_type{};
  std::uint8_t back_polygon_type{};
  ViewportState viewport{};
};

struct DrawResourceState {
  EdramMode edram_mode{EdramMode::NoOperation};
  std::array<RenderTargetDescriptor, 4> color_targets{};
  std::array<float, 4> blend_constant{};
  DepthTargetDescriptor depth_target{};
  RasterState raster{};
  std::array<std::optional<TextureDescriptor>, 32> textures{};
  std::array<std::array<std::optional<VertexBufferDescriptor>, 3>, 32>
      vertex_buffers{};
  std::uint64_t generation{};
  [[nodiscard]] std::uint64_t pipeline_hash(const ir::DrawPacket& draw) const noexcept;
};

class ResourceStateTracker {
 public:
  static constexpr std::uint32_t kFetchConstantBase = 0x4800;
  static constexpr std::uint32_t kFetchConstantDwords = 0x100;

  void reset() noexcept;
  void apply(const ir::RegisterWrite& write) noexcept;
  [[nodiscard]] std::optional<TextureDescriptor> texture(
      std::uint32_t fetch_constant) const noexcept;
  [[nodiscard]] std::optional<VertexBufferDescriptor> vertex_buffer(
      std::uint32_t fetch_constant, std::uint32_t select) const noexcept;
  [[nodiscard]] DrawResourceState snapshot() const noexcept;
  // Packs the architectural constant register banks into the stable GPU 08
  // HLSL resource ABI. Returns false if the destination is too small.
  [[nodiscard]] bool write_constant_buffer(std::span<std::byte> destination) const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

 private:
  std::array<std::uint32_t, RegisterFile::kRegisterCount> registers_{};
  std::uint64_t generation_{};
};

}  // namespace xenon::gpu
