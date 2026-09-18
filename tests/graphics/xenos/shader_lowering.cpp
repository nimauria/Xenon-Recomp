#include <cassert>
#include <iostream>
#include <string>

#include "xenon/gpu/shader_translation.hpp"

namespace {

xenon::gpu::DecodedShader make_pixel_shader(bool writes_depth) {
  using namespace xenon::gpu;
  DecodedShader shader{};
  shader.stage = ShaderStage::Pixel;
  shader.source_hash = 0x1020304050607080ull;
  shader.complete = true;
  shader.reflection.temporary_register_count = 1;
  shader.reflection.writes_depth = writes_depth;

  DecodedControlFlow cf{};
  cf.index = 0;
  cf.opcode = ControlFlowOpcode::ExecEnd;
  cf.target = 0;
  cf.count = writes_depth ? 1u : 0u;
  shader.control_flow.push_back(cf);

  if (writes_depth) {
    DecodedInstruction instruction{};
    instruction.kind = ShaderInstructionKind::Alu;
    instruction.alu.address = 0;
    instruction.alu.export_data = true;
    instruction.alu.vector_destination = 61;
    instruction.alu.scalar_destination = 7;  // Must be ignored for an export.
    instruction.alu.vector_write_mask = 0x1;
    instruction.alu.scalar_write_mask = 0x2;
    for (auto& source : instruction.alu.sources) {
      source.temporary = true;
      source.index = 0;
      source.swizzle = 0;
    }
    shader.instructions.push_back(instruction);
  }
  return shader;
}

void test_explicit_guest_depth_export() {
  using namespace xenon::gpu;
  const auto lowered = HlslShaderLowerer::lower(make_pixel_shader(true));
  assert(lowered.complete);
  assert(lowered.hlsl.find("float depth : SV_Depth") != std::string::npos);
  assert(lowered.hlsl.find("float xenon_depth = e[61].x") != std::string::npos);
  assert(lowered.hlsl.find("e[61].x = pv.x") != std::string::npos);
  assert(lowered.hlsl.find("e[61].y = ps_new.y") != std::string::npos);
  assert(lowered.hlsl.find("e[7].y = ps_new.y") == std::string::npos);
  assert(lowered.hlsl.find("xenon_quantize_float20e4(xenon_depth") ==
         std::string::npos);
}

void test_implicit_float20_depth_variant() {
  using namespace xenon::gpu;
  ShaderLoweringOptions options{};
  options.pixel_depth_output = PixelDepthOutputMode::Float20e4NearestEven;
  options.force_sample_frequency = true;
  const auto lowered = HlslShaderLowerer::lower(make_pixel_shader(false), options);
  assert(lowered.complete);
  assert(lowered.hlsl.find("uint sample_index : SV_SampleIndex") !=
         std::string::npos);
  assert(lowered.hlsl.find("float xenon_depth = input.position.z") !=
         std::string::npos);
  assert(lowered.hlsl.find("xenon_quantize_float20e4(xenon_depth, true)") !=
         std::string::npos);
  assert(lowered.hlsl.find("input.sample_index == 0xFFFFFFFFu") !=
         std::string::npos);
}

void test_float20_truncation_variant() {
  using namespace xenon::gpu;
  ShaderLoweringOptions options{};
  options.pixel_depth_output = PixelDepthOutputMode::Float20e4Truncate;
  const auto lowered = HlslShaderLowerer::lower(make_pixel_shader(false), options);
  assert(lowered.complete);
  assert(lowered.hlsl.find("xenon_quantize_float20e4(xenon_depth, false)") !=
         std::string::npos);
  assert(lowered.hlsl.find("SV_SampleIndex") == std::string::npos);
}

}  // namespace

int main() {
  test_explicit_guest_depth_export();
  test_implicit_float20_depth_variant();
  test_float20_truncation_variant();
  std::cout << "xenon_shader_lowering_tests: ok\n";
}
