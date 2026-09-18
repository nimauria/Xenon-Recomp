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



xenon::gpu::DecodedShader make_memexport_vertex_shader() {
  using namespace xenon::gpu;
  DecodedShader shader{};
  shader.stage = ShaderStage::Vertex;
  shader.source_hash = 0x4D454D4558504F52ull;
  shader.complete = true;
  shader.reflection.temporary_register_count = 1;
  shader.reflection.memory_exports = 1;
  shader.reflection.memory_export_mask = 0x1;
  shader.reflection.writes_export_address = true;
  shader.reflection.memexport_stream_constants.push_back(4);

  DecodedControlFlow exec{};
  exec.index = 0;
  exec.opcode = ControlFlowOpcode::Exec;
  exec.target = 0;
  exec.count = 2;
  shader.control_flow.push_back(exec);

  DecodedControlFlow allocation{};
  allocation.index = 1;
  allocation.opcode = ControlFlowOpcode::Alloc;
  allocation.allocation = AllocationType::Memory;
  allocation.allocation_size = 1;
  shader.control_flow.push_back(allocation);

  DecodedControlFlow end{};
  end.index = 2;
  end.opcode = ControlFlowOpcode::ExecEnd;
  end.target = 2;
  end.count = 0;
  shader.control_flow.push_back(end);

  DecodedInstruction address{};
  address.kind = ShaderInstructionKind::Alu;
  address.alu.address = 0;
  address.alu.export_data = true;
  address.alu.vector_destination = 32;
  address.alu.vector_write_mask = 0xF;
  for (auto& source : address.alu.sources) {
    source.temporary = true;
    source.index = 0;
  }
  shader.instructions.push_back(address);

  DecodedInstruction data = address;
  data.alu.address = 1;
  data.alu.vector_destination = 33;
  shader.instructions.push_back(data);
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


void test_memexport_gpu_write_lowering() {
  using namespace xenon::gpu;
  const auto lowered = HlslShaderLowerer::lower(make_memexport_vertex_shader());
  assert(lowered.complete);
  assert(lowered.hlsl.find("#define XENON_MEMORY_RW 1") != std::string::npos);
  assert(lowered.hlsl.find("RWByteAddressBuffer XenonGuestMemory : register(u0)") !=
         std::string::npos);
  assert(lowered.hlsl.find("xenon_memexport_flush") != std::string::npos);
  assert(lowered.hlsl.find("InterlockedAnd") != std::string::npos);
  assert(lowered.hlsl.find("XenonGuestMemory.Store4") != std::string::npos);
  assert(lowered.hlsl.find("memexport_written_mask |= 1u") != std::string::npos);
  assert(lowered.hlsl.find("memexport_written_mask=0u; e[32]=0.0.xxxx") !=
         std::string::npos);
}

void test_non_memexport_guest_memory_stays_read_only() {
  using namespace xenon::gpu;
  const auto lowered = HlslShaderLowerer::lower(make_pixel_shader(false));
  assert(lowered.complete);
  assert(lowered.hlsl.find("ByteAddressBuffer XenonGuestMemory : register(t0)") !=
         std::string::npos);
  assert(lowered.hlsl.find("#define XENON_MEMORY_RW 1") ==
         std::string::npos);
}


void test_forced_writable_guest_memory_variant() {
  using namespace xenon::gpu;
  ShaderLoweringOptions options{};
  options.force_guest_memory_rw = true;
  const auto lowered = HlslShaderLowerer::lower(make_pixel_shader(false), options);
  assert(lowered.complete);
  assert(lowered.hlsl.find("RWByteAddressBuffer XenonGuestMemory : register(u0)") !=
         std::string::npos);
  assert(lowered.hlsl.find("xenon_memexport_flush") == std::string::npos);
}


}  // namespace

int main() {
  test_explicit_guest_depth_export();
  test_implicit_float20_depth_variant();
  test_float20_truncation_variant();
  test_memexport_gpu_write_lowering();
  test_non_memexport_guest_memory_stays_read_only();
  test_forced_writable_guest_memory_variant();
  std::cout << "xenon_shader_lowering_tests: ok\n";
}
