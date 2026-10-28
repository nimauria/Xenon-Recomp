#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/gpu/shader_ir.hpp"
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


xenon::gpu::DecodedInstruction make_alu(std::uint32_t address) {
  using namespace xenon::gpu;
  DecodedInstruction instruction{};
  instruction.kind = ShaderInstructionKind::Alu;
  instruction.alu.address = address;
  instruction.alu.scalar_opcode = 50;
  for (auto& source : instruction.alu.sources) {
    source.temporary = true;
    source.swizzle = 0xE4;
  }
  return instruction;
}

xenon::gpu::DecodedShader make_alu_shader(xenon::gpu::ShaderStage stage,
                                          std::vector<xenon::gpu::DecodedInstruction> alus) {
  using namespace xenon::gpu;
  DecodedShader shader{};
  shader.stage = stage;
  shader.source_hash = 0x414C5553ull;
  shader.complete = true;
  shader.reflection.temporary_register_count = 4;
  DecodedControlFlow cf{};
  cf.opcode = ControlFlowOpcode::ExecEnd;
  cf.count = static_cast<std::uint32_t>(alus.size());
  shader.control_flow.push_back(cf);
  shader.instructions = std::move(alus);
  return shader;
}

// Microcode through the decoder: the AC6 boot logo's dp2add position
// transform and frcs, plus a mulsc. Scalar operations must read source 3
// (with `a` its swizzled W), never source 1.
void test_decoded_scalar_operations_read_source_three() {
  using namespace xenon::gpu;
  const std::vector<std::uint32_t> words = {
      // exec_end, four ALU instructions at slot 1.
      1u | (4u << 12), std::uint32_t(ControlFlowOpcode::ExecEnd) << 12, 0u,
      // dp2add e62.x, r0.yz, c40.xy, c40.w
      62u | (1u << 15) | (1u << 16) | (50u << 26), 0x1Bu | (0xB0u << 8) | (0xC5u << 16),
      40u | (40u << 8) | (17u << 24) | (1u << 31),
      // frcs r0.x, r0.x (vector max with an empty write mask)
      (1u << 20) | (11u << 26), 0x6Cu, (2u << 24) | (7u << 29),
      // mulsc r4.x, c9.w, r23.x
      (4u << 8) | (1u << 20) | (43u << 26), 0x14u,
      9u | (2u << 24) | (1u << 29) | (1u << 30) | (1u << 31),
      // sin r3.x, r1.y with the vector unit reading other registers
      (3u << 8) | (1u << 20) | (48u << 26), 0x01u | (0u << 8) | (0u << 16),
      1u | (2u << 8) | (0u << 16) | (2u << 24) | (7u << 29)};
  const auto decoded = ShaderDecoder::decode(ShaderProgram(ShaderStage::Vertex, words));
  assert(decoded.complete);
  const auto lowered = HlslShaderLowerer::lower(decoded);
  if (!lowered.complete) {
    for (const auto& diagnostic : lowered.diagnostics) std::cerr << diagnostic << '\n';
  }
  assert(lowered.complete);
  const auto& hlsl = lowered.hlsl;
  assert(hlsl.find("float4 s0 = r[(0) & 63].yzzz;") != std::string::npos);
  assert(hlsl.find("float4 s1 = XenonFloatConstants[((40) & 255)].xyyy;") != std::string::npos);
  assert(hlsl.find("float4 s2 = XenonFloatConstants[((40) & 255)].wwww;") != std::string::npos);
  assert(hlsl.find("float4 s2 = r[(0) & 63].xxxx;") != std::string::npos);
  assert(hlsl.find("xenon_scalar_op(11, s2, s2, ps") != std::string::npos);
  assert(hlsl.find("float4 s2 = XenonFloatConstants[((9) & 255)].xzww;") != std::string::npos);
  assert(hlsl.find("float4 s3t = r[(23) & 63].xzww;") != std::string::npos);
  assert(hlsl.find("xenon_scalar_op(43, s2, s3t, ps") != std::string::npos);
  // sin reads r1's swizzled W: relative field 1 in X makes it .yyzw... and the
  // W field 0 makes `a` = (0 + 3) & 3 = W.
  assert(hlsl.find("float4 s2 = r[(1) & 63].yyzw;") != std::string::npos);
  assert(hlsl.find("xenon_scalar_op(48, s2, s2, ps") != std::string::npos);
  assert(hlsl.find("xenon_scalar_op(48, s0") == std::string::npos);
}

// Relative temporaries add aL. Relative constants add a0 only when the ALU
// instruction's const_address_register_relative bit is set.
void test_relative_addressing_registers() {
  using namespace xenon::gpu;
  auto alu = make_alu(0);
  alu.alu.vector_write_mask = 0xF;
  alu.alu.vector_destination = 1;
  alu.alu.sources[0].relative = true;
  alu.alu.sources[1].temporary = false;
  alu.alu.sources[1].index = 12;
  alu.alu.sources[1].relative = true;
  alu.alu.constant_address_register_relative = true;
  auto loop_relative = alu;
  loop_relative.alu.address = 1;
  loop_relative.alu.constant_address_register_relative = false;
  const auto lowered = HlslShaderLowerer::lower(
      make_alu_shader(ShaderStage::Vertex, {alu, loop_relative}));
  assert(lowered.complete);
  const auto& hlsl = lowered.hlsl;
  assert(hlsl.find("float4 s0 = r[(0 + aL) & 63].xyzw;") != std::string::npos);
  assert(hlsl.find("XenonFloatConstants[((12 + a0) & 255)]") != std::string::npos);
  assert(hlsl.find("XenonFloatConstants[((12 + aL) & 255)]") != std::string::npos);
  assert(hlsl.find("+ ar") == std::string::npos);
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
  test_decoded_scalar_operations_read_source_three();
  test_relative_addressing_registers();
  std::cout << "xenon_shader_lowering_tests: ok\n";
}
