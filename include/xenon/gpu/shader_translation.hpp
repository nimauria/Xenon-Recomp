#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xenon/gpu/shader_ir.hpp"

namespace xenon::gpu {

enum class ShaderBinaryFormat : std::uint8_t { Dxil, Spirv };

struct ShaderLoweringOptions {
  std::string entry_point{"main"};
  std::uint32_t temporary_register_limit{64};
  bool emit_debug_comments{};
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

struct ShaderCompileOptions {
  ShaderBinaryFormat format{ShaderBinaryFormat::Dxil};
  bool debug{};
  bool optimize{true};
  bool warnings_as_errors{true};
  std::string spirv_environment{"vulkan1.3"};
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
