#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/primitive_processor.hpp"
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
  std::int8_t exp_adjust{};
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

struct PixelControlState {
  CompareFunction alpha_function{CompareFunction::Always};
  float alpha_reference{};
  std::array<std::uint8_t, 4> alpha_to_mask_offsets{};
  bool alpha_test_enabled{};
  bool alpha_to_mask_enabled{};
};

enum class CopyCommand : std::uint8_t {
  Raw = 0,
  Convert = 1,
  ConstantOne = 2,
  Null = 3,
};

enum class CopySampleSelect : std::uint8_t {
  Sample0 = 0,
  Sample1 = 1,
  Sample2 = 2,
  Sample3 = 3,
  Samples01 = 4,
  Samples23 = 5,
  Samples0123 = 6,
};

struct CopyResolveState {
  std::uint32_t destination_base{};
  std::uint16_t destination_pitch{};
  std::uint16_t destination_height{};
  std::uint8_t source_select{};
  CopySampleSelect sample_select{CopySampleSelect::Sample0};
  CopyCommand command{CopyCommand::Raw};
  Endian128 destination_endian{Endian128::None};
  std::uint8_t destination_slice{};
  std::uint8_t destination_format{};
  std::uint8_t destination_number_format{};
  std::int8_t destination_exponent_bias{};
  bool destination_array{};
  bool destination_red_blue_swap{};
  bool color_clear_enabled{};
  bool depth_clear_enabled{};
  std::array<std::uint32_t, 2> color_clear{};
  std::uint32_t depth_clear{};

  [[nodiscard]] bool copies_depth() const noexcept { return source_select >= 4; }
};

struct PolygonOffsetState {
  float scale{};
  float offset{};
  bool enabled{};
};

enum class HostPolygonMode : std::uint8_t {
  Fill,
  Line,
  Point,
};

struct PrimitiveAssemblyState {
  bool reset_enabled{};
  std::uint32_t reset_index{};
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
  bool vertex_window_offset_enabled{};
  bool d3d_pixel_center{true};
  std::int32_t window_offset_x{};
  std::int32_t window_offset_y{};
  bool polygon_offset_front_enabled{};
  bool polygon_offset_back_enabled{};
  bool polygon_offset_parallel_enabled{};
  std::uint8_t polygon_mode{};
  std::uint8_t front_polygon_type{};
  std::uint8_t back_polygon_type{};
  float polygon_offset_front_scale{};
  float polygon_offset_front_offset{};
  float polygon_offset_back_scale{};
  float polygon_offset_back_offset{};
  ViewportState viewport{};
};

struct NativePipelineKey {
  std::uint64_t value{};
  friend bool operator==(const NativePipelineKey&, const NativePipelineKey&) = default;
};

struct DrawResourceState {
  EdramMode edram_mode{EdramMode::NoOperation};
  std::array<RenderTargetDescriptor, 4> color_targets{};
  std::array<float, 4> blend_constant{};
  PixelControlState pixel_control{};
  CopyResolveState copy{};
  DepthTargetDescriptor depth_target{};
  PrimitiveAssemblyState primitive_assembly{};
  RasterState raster{};
  std::array<std::optional<TextureDescriptor>, 32> textures{};
  std::array<std::array<std::optional<VertexBufferDescriptor>, 3>, 32>
      vertex_buffers{};
  std::uint64_t generation{};
  [[nodiscard]] NativePipelineKey native_pipeline_key(
      const ir::DrawPacket& draw) const noexcept;
  [[nodiscard]] std::uint64_t pipeline_hash(
      const ir::DrawPacket& draw) const noexcept {
    return native_pipeline_key(draw).value;
  }
};

struct ResolveRectangle {
  std::int32_t left{};
  std::int32_t top{};
  std::int32_t right{};
  std::int32_t bottom{};
  bool valid{};

  [[nodiscard]] bool empty() const noexcept {
    return valid && (left >= right || top >= bottom);
  }
};

// Fully decoded, backend-neutral resolve policy. Native backends consume this
// instead of reinterpreting Xenos sample-selection and copy-control fields.
struct ResolvePlan {
  CopyResolveState copy{};
  ResolveRectangle rectangle{};
  MsaaSamples samples{MsaaSamples::X1};
  std::array<std::uint8_t, 4> host_sample_for_guest{0xFF, 0xFF, 0xFF, 0xFF};
  std::uint8_t guest_sample_mask{};
  std::uint8_t selected_sample_count{};
  std::uint8_t source_color_slot{};
  bool depth{};
  bool native_color_average{};
  bool valid{};
  std::string error{};
};

// Host-backend-neutral description of which Xenos color export locations are
// active for a draw. attachment_count deliberately preserves the highest
// enabled Xenos MRT slot + 1 rather than compacting the array: pixel shader
// export eN / SV_TargetN must remain attached to native color slot N.
struct ColorTargetPlan {
  std::array<bool, 4> enabled{};
  std::uint8_t enabled_mask{};
  std::uint8_t attachment_count{};
  bool contiguous{true};

  [[nodiscard]] bool any() const noexcept { return enabled_mask != 0; }
};

[[nodiscard]] ColorTargetPlan plan_color_targets(
    const DrawResourceState& state) noexcept;

// Xenos dual polygon mode can request a different representation for front and
// back faces. Modern pipelines expose one fill mode, so choose the most
// conservative visible representation (point < line < fill), matching the
// generic strategy used by native Xenos renderers. Non-dual/reserved modes are
// filled triangles.
[[nodiscard]] HostPolygonMode host_polygon_mode(
    const RasterState& raster) noexcept;

[[nodiscard]] PolygonOffsetState preferred_polygon_offset(
    const RasterState& raster, HostPrimitiveTopology topology) noexcept;

// Converts the Xenos absolute polygon offset to the units expected by modern
// floating-point host APIs. D24FS8 has three fewer mantissa bits than float32,
// while D24S8 uses the full unsigned 24-bit depth range.
[[nodiscard]] float scaled_polygon_offset_constant(
    float offset, DepthRenderTargetFormat format) noexcept;

// Converts the Xenos absolute polygon offset to the integer ULP count used by
// D3D12 rasterizer state. Float24 offsets are kept in multiples of eight so
// shader-side float24 quantization cannot erase the separation.
[[nodiscard]] std::int32_t integer_polygon_offset(
    float offset, DepthRenderTargetFormat format) noexcept;

[[nodiscard]] CopySampleSelect sanitize_copy_sample_select(
    CopySampleSelect selection, MsaaSamples samples, bool depth) noexcept;

// Native image resolve operations average every color sample. This identifies
// the Xenos selections that are bit-for-bit representable by that operation.
[[nodiscard]] bool is_full_color_resolve(CopySampleSelect selection,
                                         MsaaSamples samples) noexcept;

// D3D rasterization converts window coordinates to signed 16.8 fixed point
// with round-to-nearest-even, NaN-to-zero and saturation. Keeping this helper
// host-independent prevents one-pixel resolve/scissor differences.
[[nodiscard]] std::int32_t float_to_d3d_fixed_16_8(float value) noexcept;

// Extracts the covered copy-mode rectangle from the conventional three-float2
// Xenos resolve vertex stream, applies pixel-center/window/scissor state, and
// expands it to the hardware's 8x8 resolve granularity.
[[nodiscard]] ResolveRectangle decode_resolve_rectangle(
    const DrawResourceState& state,
    std::span<const std::byte> physical_memory) noexcept;

[[nodiscard]] ResolvePlan plan_resolve(
    const DrawResourceState& state,
    std::span<const std::byte> physical_memory);

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
