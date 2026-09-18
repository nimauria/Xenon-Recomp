#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>

#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"

namespace {

xenon::gpu::DecodedShader make_shader(xenon::gpu::ShaderStage stage) {
  using namespace xenon::gpu;
  DecodedShader shader{};
  shader.stage = stage;
  shader.source_hash = stage == ShaderStage::Vertex ? 0x123456789ABCDEF0ull
                                                    : 0x0FEDCBA987654321ull;
  shader.complete = true;
  shader.reflection.temporary_register_count = 1;
  shader.reflection.exports = {stage == ShaderStage::Vertex ? std::uint8_t{62}
                                                             : std::uint8_t{0}};

  DecodedControlFlow cf{};
  cf.opcode = ControlFlowOpcode::ExecEnd;
  cf.target = 1;
  cf.count = 1;
  shader.control_flow.push_back(cf);

  DecodedInstruction instruction{};
  instruction.kind = ShaderInstructionKind::Alu;
  instruction.alu.address = 1;
  instruction.alu.vector_opcode = 0;
  instruction.alu.scalar_opcode = 50;
  instruction.alu.vector_destination = stage == ShaderStage::Vertex ? 62 : 0;
  instruction.alu.vector_write_mask = 0xF;
  instruction.alu.export_data = true;
  instruction.alu.sources[0].temporary = true;
  instruction.alu.sources[0].index = 0;
  instruction.alu.sources[0].swizzle = 0xE4;
  instruction.alu.sources[1] = instruction.alu.sources[0];
  instruction.alu.sources[2] = instruction.alu.sources[0];
  shader.instructions.push_back(instruction);
  return shader;
}

void test_lowering_is_deterministic() {
  using namespace xenon::gpu;
  const auto shader = make_shader(ShaderStage::Vertex);
  const auto first = HlslShaderLowerer::lower(shader);
  const auto second = HlslShaderLowerer::lower(shader);
  assert(first.complete);
  assert(first.hlsl == second.hlsl);
  assert(first.translation_hash == second.translation_hash);
  assert(first.profile == "vs_6_0");
  assert(first.hlsl.find("XenonFloatConstants") != std::string::npos);
  assert(first.hlsl.find("output.position=e[62]") != std::string::npos);
}

void test_dxc_dual_target_and_cache() {
  using namespace xenon::gpu;
  auto compiler = std::make_shared<DxcShaderCompiler>();
  assert(compiler->available());
  ShaderCache cache(compiler);
  const auto lowered = HlslShaderLowerer::lower(make_shader(ShaderStage::Vertex));

  ShaderCompileOptions dxil_options{};
  dxil_options.format = ShaderBinaryFormat::Dxil;
  const auto dxil = cache.get_or_compile(lowered, dxil_options);
  if (!dxil->succeeded) {
    for (const auto& diagnostic : dxil->diagnostics) std::cerr << diagnostic << '\n';
  }
  assert(dxil->succeeded && dxil->binary.size() > 4);
  assert(std::memcmp(dxil->binary.data(), "DXBC", 4) == 0);
  const auto dxil_again = cache.get_or_compile(lowered, dxil_options);
  assert(dxil == dxil_again);

  ShaderCompileOptions spirv_options{};
  spirv_options.format = ShaderBinaryFormat::Spirv;
  const auto spirv = cache.get_or_compile(lowered, spirv_options);
  if (!spirv->succeeded) {
    for (const auto& diagnostic : spirv->diagnostics) std::cerr << diagnostic << '\n';
  }
  assert(spirv->succeeded && spirv->binary.size() > 4);
  std::uint32_t magic{};
  std::memcpy(&magic, spirv->binary.data(), sizeof(magic));
  assert(magic == 0x07230203u);
  assert(cache.size() == 2 && cache.hits() == 1 && cache.misses() == 2);
}

void test_pixel_target_and_incomplete_rejection() {
  using namespace xenon::gpu;
  DxcShaderCompiler compiler;
  auto pixel_shader = make_shader(ShaderStage::Pixel);
  pixel_shader.instructions[0].alu.sources[0].temporary = false;
  const auto pixel = HlslShaderLowerer::lower(pixel_shader);
  assert(pixel.complete && pixel.profile == "ps_6_0");
  assert(pixel.hlsl.find("& 255) + 256]") != std::string::npos);
  assert(pixel.hlsl.find("uint coverage : SV_Coverage") != std::string::npos);
  assert(pixel.hlsl.find("xenon_compare(e[0].w") != std::string::npos);
  assert(pixel.hlsl.find("xenon_alpha_to_mask(e[0].w") != std::string::npos);
  assert(pixel.hlsl.find("0.75 - o / 16.0") != std::string::npos);
  assert(compiler.compile(pixel, {}).succeeded);

  pixel_shader.reflection.exports.clear();
  const auto no_color_zero = HlslShaderLowerer::lower(pixel_shader);
  assert(no_color_zero.complete);
  assert(no_color_zero.hlsl.find("xenon_compare(e[0].w") == std::string::npos);
  assert(no_color_zero.hlsl.find("output.coverage = 0xFFFFFFFFu") !=
         std::string::npos);
  assert(compiler.compile(no_color_zero, {}).succeeded);

  DecodedShader incomplete{};
  incomplete.diagnostics.emplace_back("fixture failure");
  const auto rejected = HlslShaderLowerer::lower(incomplete);
  assert(!rejected.complete && !rejected.diagnostics.empty());
}

void test_control_flow_state_machine_and_explicit_rejection() {
  using namespace xenon::gpu;
  auto shader = make_shader(ShaderStage::Vertex);
  shader.control_flow.clear();
  DecodedControlFlow loop_start{};
  loop_start.index = 0;
  loop_start.opcode = ControlFlowOpcode::LoopStart;
  loop_start.target = 3;
  loop_start.loop_constant = 2;
  shader.control_flow.push_back(loop_start);
  DecodedControlFlow body{};
  body.index = 1;
  body.opcode = ControlFlowOpcode::Exec;
  body.target = 1;
  body.count = 1;
  shader.control_flow.push_back(body);
  DecodedControlFlow loop_end{};
  loop_end.index = 2;
  loop_end.opcode = ControlFlowOpcode::LoopEnd;
  loop_end.target = 1;
  loop_end.loop_constant = 2;
  shader.control_flow.push_back(loop_end);
  DecodedControlFlow end = body;
  end.index = 3;
  end.opcode = ControlFlowOpcode::ExecEnd;
  shader.control_flow.push_back(end);
  const auto lowered = HlslShaderLowerer::lower(shader);
  assert(lowered.complete);
  assert(lowered.hlsl.find("switch (pc)") != std::string::npos);
  assert(lowered.hlsl.find("loop_remaining") != std::string::npos);
  DxcShaderCompiler compiler;
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  assert(compiler.compile(lowered, spirv).succeeded);

  auto unsupported = make_shader(ShaderStage::Pixel);
  unsupported.instructions[0].kind = ShaderInstructionKind::TextureFetch;
  unsupported.instructions[0].texture_fetch.address = 1;
  unsupported.instructions[0].texture_fetch.opcode = 17;
  const auto rejected = HlslShaderLowerer::lower(unsupported);
  assert(!rejected.complete && !rejected.diagnostics.empty());
}

void test_texture_lod_and_gradient_lowering() {
  using namespace xenon::gpu;
  auto shader = make_shader(ShaderStage::Pixel);
  auto& instruction = shader.instructions[0];
  instruction.kind = ShaderInstructionKind::TextureFetch;
  instruction.texture_fetch.address = 1;
  instruction.texture_fetch.opcode = 1;
  instruction.texture_fetch.fetch_constant = 3;
  instruction.texture_fetch.dimension = 1;
  instruction.texture_fetch.source_register = 4;
  instruction.texture_fetch.destination_register = 5;
  instruction.texture_fetch.destination_swizzle = 0x688;
  instruction.texture_fetch.use_register_gradients = true;
  instruction.texture_fetch.unnormalized_coordinates = true;
  instruction.texture_fetch.offsets_half_texels = {1, -1, 0};
  shader.reflection.texture_fetch_constants = {3};
  const auto lowered = HlslShaderLowerer::lower(shader);
  assert(lowered.complete);
  assert(lowered.hlsl.find("SampleGrad") != std::string::npos);
  assert(lowered.hlsl.find("true, grad_h, grad_v, true") != std::string::npos);
  assert(lowered.hlsl.find("xenon_apply_texture_exp_adjust(3, fetched)") !=
         std::string::npos);
  assert(lowered.hlsl.find(
             "XenonVertexFetchConstants[fetch_constant & 31].y") !=
         std::string::npos);
  DxcShaderCompiler compiler;
  assert(compiler.compile(lowered, {}).succeeded);
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  assert(compiler.compile(lowered, spirv).succeeded);
}

void test_rectangle_list_geometry_shader() {
  using namespace xenon::gpu;
  const auto lowered = make_rectangle_list_geometry_shader();
  assert(lowered.complete && lowered.stage == ShaderStage::Geometry);
  assert(lowered.profile == "gs_6_0");
  assert(lowered.hlsl.find("d01 >= d12") != std::string::npos);
  assert(lowered.hlsl.find("maxvertexcount(4)") != std::string::npos);
  DxcShaderCompiler compiler;
  assert(compiler.compile(lowered, {}).succeeded);
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  assert(compiler.compile(lowered, spirv).succeeded);
}

void test_sample_transfer_shaders() {
  using namespace xenon::gpu;
  DxcShaderCompiler compiler;
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  for (const auto& shader : {make_transfer_fullscreen_vertex_shader(),
                             make_color_sample_read_shader(MsaaSamples::X2),
                             make_color_sample_read_shader(MsaaSamples::X4),
                             make_color_sample_write_shader()}) {
    assert(shader.complete);
    assert(compiler.compile(shader, {}).succeeded);
    assert(compiler.compile(shader, spirv).succeeded);
  }
  assert(make_color_sample_read_shader(MsaaSamples::X2).hlsl.find(
             "Texture2DMS<float4,2>") != std::string::npos);
  assert(make_color_sample_write_shader().hlsl.find("SV_SampleIndex") !=
         std::string::npos);
  for (const auto samples : {MsaaSamples::X1, MsaaSamples::X2, MsaaSamples::X4}) {
    const auto read = make_depth_sample_read_shader(samples);
    assert(read.complete);
    assert(compiler.compile(read, {}).succeeded);
    assert(compiler.compile(read, spirv).succeeded);
  }
  const auto write = make_depth_sample_write_shader();
  assert(write.complete && write.hlsl.find("SV_StencilRef") != std::string::npos);
  assert(compiler.compile(write, {}).succeeded);
  spirv.spirv_stencil_export = true;
  assert(compiler.compile(write, spirv).succeeded);
  for (const auto& fallback : {make_depth_only_sample_write_shader(),
                               make_stencil_mask_write_shader()}) {
    assert(fallback.complete);
    assert(compiler.compile(fallback, {}).succeeded);
    assert(compiler.compile(fallback, spirv).succeeded);
  }
}


void test_memexport_translation_targets() {
  using namespace xenon::gpu;
  auto shader = make_shader(ShaderStage::Vertex);
  shader.reflection.memory_exports = 1;
  shader.reflection.memory_export_mask = 1;
  shader.reflection.writes_export_address = true;
  const auto lowered = HlslShaderLowerer::lower(shader);
  assert(lowered.complete);
  assert(lowered.hlsl.find("#define XENON_MEMORY_RW 1") != std::string::npos);
  assert(lowered.hlsl.find("xenon_memexport_flush") != std::string::npos);

  DxcShaderCompiler compiler;
  ShaderCompileOptions dxil{};
  assert(compiler.compile(lowered, dxil).succeeded);
  ShaderCompileOptions spirv{};
  spirv.format = ShaderBinaryFormat::Spirv;
  assert(compiler.compile(lowered, spirv).succeeded);

  auto companion = make_shader(ShaderStage::Vertex);
  ShaderLoweringOptions rw_options{};
  rw_options.force_guest_memory_rw = true;
  const auto rw = HlslShaderLowerer::lower(companion, rw_options);
  assert(rw.complete);
  assert(rw.hlsl.find("#define XENON_MEMORY_RW 1") != std::string::npos);
  assert(rw.hlsl.find("xenon_memexport_flush") == std::string::npos);
  assert(compiler.compile(rw, dxil).succeeded);
  assert(compiler.compile(rw, spirv).succeeded);
}


}  // namespace

int main() {
  test_lowering_is_deterministic();
  test_dxc_dual_target_and_cache();
  test_pixel_target_and_incomplete_rejection();
  test_control_flow_state_machine_and_explicit_rejection();
  test_texture_lod_and_gradient_lowering();
  test_rectangle_list_geometry_shader();
  test_sample_transfer_shaders();
  test_memexport_translation_targets();
  std::cout << "xenon_shader_translation_tests: ok\n";
}
