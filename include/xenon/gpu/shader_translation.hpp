#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xenon/gpu/shader_ir.hpp"
#include "xenon/gpu/edram_surface.hpp"

namespace xenon::gpu {

enum class ShaderBinaryFormat : std::uint8_t { Dxil, Spirv };

enum class PixelDepthOutputMode : std::uint8_t {
  Native,
  Float20e4NearestEven,
  Float20e4Truncate,
};

struct ShaderLoweringOptions {
  std::string entry_point{"main"};
  std::uint32_t temporary_register_limit{64};
  bool emit_debug_comments{};
  PixelDepthOutputMode pixel_depth_output{PixelDepthOutputMode::Native};
  bool force_sample_frequency{};
};

struct LoweredShader {
  ShaderStage stage{ShaderStage::Vertex};
  std::uint64_t source_hash{};
  std::uint64_t translation_hash{};
  std::string entry_point{};
  std::string profile{};
  std::string hlsl{};
  ShaderReflection reflection{};
  std::vector<std::string> diagnostics{};
  bool complete{};
};

class HlslShaderLowerer {
 public:
  [[nodiscard]] static LoweredShader lower(
      const DecodedShader& shader, const ShaderLoweringOptions& options = {});
};

// Creates the host-only stage that expands one Xenos RectangleList primitive
// (three post-VS corners) to a four-corner triangle strip. This is shared by
// Vulkan and D3D12 and contains no guest command processing.
[[nodiscard]] LoweredShader make_rectangle_list_geometry_shader();

// Host-only shaders used by both backends for exact EDRAM ownership transfer.
// Unlike a native resolve, the read shader loads one explicitly mapped sample;
// the write shader updates only that sample via SV_SampleIndex.
[[nodiscard]] LoweredShader make_transfer_fullscreen_vertex_shader();
[[nodiscard]] LoweredShader make_color_sample_read_shader(MsaaSamples samples);
[[nodiscard]] LoweredShader make_color_sample_write_shader();
[[nodiscard]] LoweredShader make_depth_sample_read_shader(MsaaSamples samples);
[[nodiscard]] LoweredShader make_depth_sample_write_shader();
[[nodiscard]] LoweredShader make_depth_only_sample_write_shader();
[[nodiscard]] LoweredShader make_stencil_mask_write_shader();

struct ShaderCompileOptions {
  ShaderBinaryFormat format{ShaderBinaryFormat::Dxil};
  bool debug{};
  bool optimize{true};
  bool warnings_as_errors{true};
  std::string spirv_environment{"vulkan1.3"};
  bool spirv_stencil_export{};
};

struct CompiledShader {
  ShaderStage stage{ShaderStage::Vertex};
  ShaderBinaryFormat format{ShaderBinaryFormat::Dxil};
  std::uint64_t source_hash{};
  std::uint64_t cache_key{};
  std::string entry_point{};
  std::string profile{};
  std::vector<std::byte> binary{};
  std::vector<std::string> diagnostics{};
  bool succeeded{};
};

}  // namespace xenon::gpu
